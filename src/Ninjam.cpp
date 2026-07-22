// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "plugin.hpp"
#include "net/Stream.hpp"
#include "net/RoomDirectory.hpp"
#include "net/ninjam/NjClient.hpp"
#include "ClickableLed.hpp"
#include "Theme.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>
#include <vector>
#ifndef ARCH_WIN
#include <sys/stat.h> // chmod: keep the plaintext-password config owner-only
#endif

// NINJAM module — two ways to be in a jam:
//
//  • LISTEN (Icecast stream): the zero-dependency path. Public NINJAM communities
//    (ninjamer.com, ninbot, …) publish a live Icecast/HTTP mix of each room, so we
//    consume it like internet radio (net/Stream.hpp, same as Radio). No protocol, no
//    join — pure external listening.
//  • JOIN (NINJAM protocol): connect to the server over the real NINJAM protocol
//    (net/ninjam/NjClient), authenticate, subscribe, and decode the live multi-user
//    OGG interval mix ourselves — plus TRANSMIT: with TX enabled, the poly input
//    jacks are captured, OGG-Vorbis-encoded per interval, and uploaded so the room
//    hears us. Room chat works both ways.
//
// Both feed the same lock-free ring → process() just pull()s from whichever is active.
// Split a "host:port" string (port defaults to the NINJAM default 2049 if absent/bad).
static void parseHostPort(const std::string& s, std::string& host, int& port) {
	size_t colon = s.rfind(':');
	if (colon != std::string::npos) {
		host = s.substr(0, colon);
		port = std::atoi(s.substr(colon + 1).c_str());
	} else {
		host = s;
		port = 0;
	}
	if (port <= 0)
		port = 2049;
}

struct Ninjam : Module {
	enum ParamId {
		PARAMS_LEN
	};
	enum InputId {
		LEFT_INPUT,  // transmit: poly L (channel n = local instrument chain n)
		RIGHT_INPUT, // transmit: poly R
		INPUTS_LEN
	};
	enum OutputId {
		LEFT_OUTPUT,   // MAIN L (full mix)
		RIGHT_OUTPUT,  // MAIN R
		POLY_L_OUTPUT, // per-player bundle, left  (channel n = player n)
		POLY_R_OUTPUT, // per-player bundle, right
		CLICK_OUTPUT,  // metronome click
		CLOCK_OUTPUT,  // gate per beat
		RESET_OUTPUT,  // trigger at interval downbeat
		RUN_OUTPUT,    // high while playing
		PHASE_OUTPUT,  // 0-10V ramp across the interval
		OUTPUTS_LEN
	};
	enum LightId {
		CONNECTED_LIGHT,
		LIGHTS_LEN
	};

	enum Mode { MODE_LISTEN = 0, MODE_JOIN = 1 };

	akaudio::StreamClient stream;
	// Background directory of public jam rooms (ninbot). Fetched off-thread; the
	// UI reads its cache instantly. Not persisted — rooms come and go.
	akaudio::RoomDirectory directory;

	// UI-thread-written flags that process() (audio thread) also reads: atomics,
	// so the cross-thread reads are well-defined. UI code uses them like plain
	// values via std::atomic's implicit conversions; process() loads relaxed.
	std::atomic<int> mode{MODE_LISTEN};

	// ---- LISTEN (Icecast) state ----
	// The jam's Icecast/HTTP listen-stream URL (MP3). Set by picking a room or typing
	// one; empty by default since rooms come and go.
	std::string url = "";
	// A second listen mount to try if `url` fails to connect: some rooms advertise a
	// plain-http mount on a non-standard port that is firewalled/dead while their
	// https mount (or vice-versa) is live. playUrl() prefers plain http, so this holds
	// the room's ssl_stream (when it differs) as a one-shot fallback. Cleared once the
	// primary connects or the fallback has been tried.
	std::string listenAltUrl = "";
	std::atomic<bool> listening{false};

	// ---- JOIN (protocol) state ----
	// The string fields are UI-thread-only (NjClient callbacks capture copies).
	std::string joinHost = "";
	int joinPort = 2049;
	std::string joinUser = "";  // display name; empty -> anonymous
	std::string joinPass = "";  // empty -> anonymous; set for a registered/private server
	std::atomic<bool> joined{false};
	// Most-recent-first "host:port" of servers we've joined, for the panel dropdown.
	std::vector<std::string> serverHistory;
	// Per-server credentials keyed by "host:port". Persisted ONLY in the global file
	// (loadGlobal/saveGlobal), never in a patch — a shared .vcv carries no password.
	struct ServerCred { std::string user, pass; };
	std::map<std::string, ServerCred> serverCreds;

	// ---- Beat clock + metronome (meaningful only when joined to a tempo) ----
	std::atomic<bool> clickEnabled{false}; // metronome audible-click toggle (persisted)
	// CLOCK output resolution in pulses per beat (PPQN). 1 = 1 pulse/beat (a step
	// sequencer steps once per beat); 2 (default) suits modules that detect tempo
	// assuming 2 pulses/beat; 24 ≈ MIDI-clock-style sync. Persisted.
	std::atomic<int> clockPpqn{2};
	std::atomic<int> currentBeat{-1};     // UI reads this; -1 = idle
	std::atomic<float> jamPhase{0.f};     // 0..1 interval progress (UI progress bar)
	std::atomic<bool> resyncBeat{false};  // set on join / tempo change to reset the clock
	// process()-thread only beat/click state:
	double beatPhase = 0.0;
	int beatIndex = 0;
	float clickEnv = 0.f, clickPhase = 0.f, clickFreq = 880.f, clickAmp = 0.f;
	// Cached per-rate/per-tempo values so process() pays a compare per frame instead of
	// an exp() (peak release) and a division (samples per beat):
	float cachedRate = 0.f;
	float peakDecay = 0.f;
	double spb = 0.0; // samples per beat
	int spbBpm = 0;

	// Human label for the picked room (room name, or host). Shown on the panel and
	// persisted; shared across modes since only one is active at a time.
	std::string roomLabel = "";

	// Live jam metadata from the protocol client (written by its net thread).
	std::atomic<int> jamBpm{0};
	std::atomic<int> jamBpi{0};
	std::mutex rosterMutex;
	std::vector<std::string> roster; // "name" of active remote users (UI thread reads)

	// ---- Unexpected-disconnect reporting ----
	// The network thread (onChat/onState) writes the raw reason; the UI thread reads it in
	// handleServerDrop(). dropMutex guards both. `lastServerMsg` = the most recent server
	// announcement (empty-sender MSG) — for a kick this is the "User X kicked by Y" line the
	// server broadcasts just before dropping us. `termMsg` = the client's terminal Error/
	// Disconnected message. `disconnectNote` (UI thread only) = the reason shown on the status
	// bar after we auto-return to the connect view; "" when there is nothing to report.
	std::mutex dropMutex;
	std::string lastServerMsg;
	std::string termMsg;
	std::string disconnectNote;

	// ---- Chat ----
	// Room chat log. The network thread (onChat) appends; the UI thread reads a snapshot.
	// Guarded by chatMutex. Declared before njclient so it outlives its callbacks.
	struct ChatLine {
		enum Kind { Normal, System, Topic };
		std::string who;
		std::string text;
		Kind kind;
		bool mine;  // a named message we sent ourselves (own-vs-others colour)
		ChatLine(std::string w, std::string t, Kind k = Normal, bool mine_ = false)
			: who(std::move(w)), text(std::move(t)), kind(k), mine(mine_) {}
	};
	std::mutex chatMutex;
	std::vector<ChatLine> chatLog;
	std::atomic<unsigned> chatGen{0}; // bumped on append so the UI can auto-scroll to newest
	static constexpr size_t CHAT_MAX = 200;
	void pushChat(ChatLine l) {
		// Defensive caps: a hostile/buggy server can send arbitrarily long chat, topic,
		// or name strings (NINJAM frames run up to 16 MiB). Without a bound, 200 retained
		// lines could hold ~GBs and the UI thread's per-frame wrapText would stall the GUI.
		static constexpr size_t MAX_WHO = 128, MAX_TEXT = 2000;
		if (l.who.size() > MAX_WHO)
			l.who.resize(MAX_WHO);
		if (l.text.size() > MAX_TEXT) {
			l.text.resize(MAX_TEXT);
			l.text += "\xe2\x80\xa6"; // "…"
		}
		std::lock_guard<std::mutex> lk(chatMutex);
		chatLog.push_back(std::move(l));
		if (chatLog.size() > CHAT_MAX)
			chatLog.erase(chatLog.begin(), chatLog.begin() + (chatLog.size() - CHAT_MAX));
		chatGen.fetch_add(1, std::memory_order_release);
	}
	std::vector<ChatLine> chatSnapshot() {
		std::lock_guard<std::mutex> lk(chatMutex);
		return chatLog;
	}
	// True if `speaker` is `mine`. Registered logins echo our username verbatim;
	// anonymous logins come back as "anonymous:<name>" (or bare "anonymous" when we
	// gave no name), so strip that prefix before the case-insensitive compare.
	// `mine` is passed in (a copy captured when the session's callbacks were built)
	// rather than read from joinUser, because this runs on the NET thread — reading
	// the member would race the UI thread editing the username field.
	static bool isOwnSpeaker(std::string speaker, const std::string& mine) {
		const std::string ap = "anonymous:";
		if (speaker.rfind(ap, 0) == 0)
			speaker = speaker.substr(ap.size());
		if (speaker.size() != mine.size())
			return false;
		for (size_t i = 0; i < speaker.size(); i++)
			if (std::tolower((unsigned char) speaker[i]) != std::tolower((unsigned char) mine[i]))
				return false;
		return true;
	}
	// Route a typed chat line the way JamTaba does:
	//   /bpm /bpi /topic /kick  -> ADMIN command (server acts only if we're admin)
	//   /msg <user> <text>      -> private message
	//   !vote bpm|bpi <n>       -> ordinary public message (the public-server vote path)
	//   anything else           -> ordinary public message
	void sendChatLine(const std::string& text) {
		if (text.empty())
			return;
		auto startsWith = [&](const char* p) { return text.rfind(p, 0) == 0; };
		if (startsWith("/bpm") || startsWith("/bpi") || startsWith("/topic") || startsWith("/kick")) {
			njclient.sendAdmin(text.substr(1)); // strip the leading '/'
		} else if (startsWith("/msg ")) {
			std::string rest = text.substr(5);
			size_t sp = rest.find(' ');
			if (sp != std::string::npos)
				njclient.sendPrivate(rest.substr(0, sp), rest.substr(sp + 1));
		} else {
			njclient.sendChat(text); // public MSG; server echoes it back to us
		}
	}

	// Decaying output peak [0,1] for the on-panel level meter. Written by the audio
	// thread, read by the UI thread; relaxed atomics are fine for a meter.
	std::atomic<float> peak{0.f};

	// ---- Transmit ----
	std::atomic<bool> transmitting{false}; // TX enable (auto-on at explicit join; NOT persisted)
	// One-shot "you're silent!" nudge: set only by the patch-load auto-rejoin (the one
	// join path that deliberately leaves TX off), shown by the widget as a big yellow
	// call-to-action until the user makes any TX decision (or leaves the room).
	std::atomic<bool> txNudge{false};
	std::atomic<float> txQuality{0.5f};    // encoder VBR quality (persisted; ~190 kbps)
	std::atomic<bool> txVoice{false};      // voice-chat mode: live, unsynced (persisted setting)
	std::atomic<int> txDeclared{-1};       // last channel count declared (written on UI thread)
	std::atomic<bool> txArmed{false};      // capture armed at a beat boundary (aligns intervals)

	// Declared last so it is destroyed FIRST: NjClient::~ joins its threads before the
	// state its callbacks touch (roster/atomics above) is torn down.
	akaudio::nj::NjClient njclient;

	Ninjam() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configOutput(LEFT_OUTPUT, "Main mix L");
		configOutput(RIGHT_OUTPUT, "Main mix R");
		configOutput(POLY_L_OUTPUT, "Players L (poly: channel = player)");
		configOutput(POLY_R_OUTPUT, "Players R (poly: channel = player)");
		configOutput(CLICK_OUTPUT, "Metronome click");
		configOutput(CLOCK_OUTPUT, "Clock (per beat)");
		configOutput(RESET_OUTPUT, "Reset (interval downbeat)");
		configOutput(RUN_OUTPUT, "Run (high while playing)");
		configOutput(PHASE_OUTPUT, "Interval phase (0-10V ramp)");
		configInput(LEFT_INPUT, "Transmit L (poly: channel = your channel)");
		configInput(RIGHT_INPUT, "Transmit R (poly: channel = your channel)");
		float sr = APP->engine->getSampleRate();
		stream.setSampleRate(sr);
		njclient.setSampleRate(sr);
		loadGlobal(); // prefill a fresh module from the last-used server/creds/history
		// NOTE: do NOT fetch the room directory here. Contacting ninbot.com must be
		// user-initiated (Refresh, a click in the room list, or focusing search), so
		// merely adding the module or opening a patch that contains one never phones home.
	}

	// Point the active join fields (host/port/user/pass) at a stored "host:port".
	void applyServer(const std::string& hp) {
		std::string host; int port = 2049;
		parseHostPort(hp, host, port);
		joinHost = host;
		joinPort = port > 0 ? port : 2049;
		auto it = serverCreds.find(hp);
		joinUser = it != serverCreds.end() ? it->second.user : "";
		joinPass = it != serverCreds.end() ? it->second.pass : "";
	}

	// Cross-instance recall of joined servers + their credentials. Persisted ONLY here,
	// in the Rack user dir — a saved patch (dataToJson) carries NONE of this, so a shared
	// .vcv can't leak the user's NINJAM password, username, or private-server list.
	static std::string globalConfigPath() { return asset::user("akaudio-ninjam.json"); }

	void loadGlobal() {
		json_error_t err;
		json_t* root = json_load_file(globalConfigPath().c_str(), 0, &err);
		if (!root)
			return;
		serverHistory.clear();
		serverCreds.clear();
		if (json_t* servers = json_object_get(root, "servers")) {
			// Current format: ordered array of {host, port, user, pass}, most-recent-first.
			size_t i; json_t* v;
			json_array_foreach(servers, i, v) {
				std::string host = jsonStr(json_object_get(v, "host"));
				if (host.empty())
					continue;
				int port = 2049;
				if (json_t* p = json_object_get(v, "port"))
					port = (int) json_integer_value(p);
				std::string hp = host + ":" + std::to_string(port);
				serverHistory.push_back(hp);
				serverCreds[hp] = { jsonStr(json_object_get(v, "user")),
				                    jsonStr(json_object_get(v, "pass")) };
			}
		} else {
			// Legacy format (pre-2026-07): one flat credential + a bare host:port history.
			// Migrate it into the keyed store so an existing user keeps their saved login.
			std::string host = jsonStr(json_object_get(root, "joinHost"));
			int port = 2049;
			if (json_t* p = json_object_get(root, "joinPort"))
				port = (int) json_integer_value(p);
			std::string user = jsonStr(json_object_get(root, "joinUser"));
			std::string pass = jsonStr(json_object_get(root, "joinPass"));
			if (json_t* hist = json_object_get(root, "serverHistory")) {
				size_t i; json_t* v;
				json_array_foreach(hist, i, v)
					if (const char* s = json_string_value(v)) serverHistory.push_back(s);
			}
			if (!host.empty()) {
				std::string hp = host + ":" + std::to_string(port);
				if (std::find(serverHistory.begin(), serverHistory.end(), hp) == serverHistory.end())
					serverHistory.insert(serverHistory.begin(), hp);
				serverCreds[hp] = { user, pass };
			}
		}
		json_decref(root);
		if (!serverHistory.empty())
			applyServer(serverHistory.front()); // prefill from the most-recent server
	}

	void saveGlobal() {
		json_t* root = json_object();
		json_t* servers = json_array();
		for (const std::string& hp : serverHistory) {
			std::string host; int port = 2049;
			parseHostPort(hp, host, port);
			json_t* e = json_object();
			json_object_set_new(e, "host", json_string(host.c_str()));
			json_object_set_new(e, "port", json_integer(port));
			auto it = serverCreds.find(hp);
			json_object_set_new(e, "user", json_string(it != serverCreds.end() ? it->second.user.c_str() : ""));
			json_object_set_new(e, "pass", json_string(it != serverCreds.end() ? it->second.pass.c_str() : ""));
			json_array_append_new(servers, e);
		}
		json_object_set_new(root, "servers", servers);
		json_dump_file(root, globalConfigPath().c_str(), JSON_INDENT(2));
		json_decref(root);
#ifndef ARCH_WIN
		// The file holds the plaintext NINJAM password — keep it owner-read/write only.
		chmod(globalConfigPath().c_str(), S_IRUSR | S_IWUSR);
#endif
	}

	// NjClient callbacks (fire on its background thread). Keep them light.
	akaudio::nj::NjClient::Callbacks jamCallbacks() {
		akaudio::nj::NjClient::Callbacks cb;
		// Log only the abnormal (errors / unexpected disconnects). onLog only fires for
		// anomalies; onState logs only the Error state. Normal connect/auth/subscribe/stop
		// stay silent. (Verbose per-event INFO logging available behind the commented line.)
		cb.onState = [this](akaudio::nj::NjClient::State s, const std::string& msg) {
			using State = akaudio::nj::NjClient::State;
			if (s == State::Error || s == State::Disconnected) {
				std::lock_guard<std::mutex> lk(dropMutex);
				termMsg = msg; // the terminal reason; handleServerDrop() surfaces it
			}
			if (s == State::Error)
				WARN("akaudio.Ninjam: %s", msg.c_str());
			// INFO("akaudio.Ninjam: state=%s %s", akaudio::nj::stateName(s), msg.c_str());
		};
		cb.onLog = [](const std::string& m) { WARN("akaudio.Ninjam: %s", m.c_str()); };
		cb.onConfig = [this](int bpm, int bpi) {
			jamBpm.store(bpm, std::memory_order_relaxed);
			jamBpi.store(bpi, std::memory_order_relaxed);
			resyncBeat.store(true, std::memory_order_release); // realign the beat clock
		};
		cb.onUserInfo = [this](const std::vector<akaudio::nj::UserChannel>& users) {
			std::lock_guard<std::mutex> lock(rosterMutex);
			for (const auto& u : users) {
				auto it = std::find(roster.begin(), roster.end(), u.user);
				if (u.active && it == roster.end())
					roster.push_back(u.user);
				else if (!u.active && it != roster.end())
					roster.erase(it);
			}
		};
		// Snapshot of our display name for isOwnSpeaker(): the onChat lambda runs on
		// the net thread, so it must not read the joinUser member (the UI edits it).
		const std::string self = joinUser.empty() ? std::string("anonymous") : joinUser;
		cb.onChat = [this, self](const akaudio::nj::ChatMessage& m) {
			const std::string& c = m.cmd();
			// A server message (MSG with an empty sender) carries server-side announcements —
			// notably the "User X kicked by Y" line the server broadcasts right before it drops
			// a kicked client. Stash the latest so an unexpected disconnect can report the reason.
			if (c == "MSG" && m.arg(0).empty() && !m.arg(1).empty()) {
				std::lock_guard<std::mutex> lk(dropMutex);
				lastServerMsg = m.arg(1);
			}
			if (c == "MSG")          pushChat({m.arg(0), m.arg(1), ChatLine::Normal, isOwnSpeaker(m.arg(0), self)}); // speaker, text
			else if (c == "PRIVMSG") pushChat({m.arg(0) + " (pm)", m.arg(1), ChatLine::Normal, isOwnSpeaker(m.arg(0), self)});
			else if (c == "TOPIC") {
				if (!m.arg(1).empty())
					pushChat({"", (m.arg(0).empty() ? std::string("Topic: ") : m.arg(0) + " set topic: ") + m.arg(1), ChatLine::Topic});
			}
			else if (c == "JOIN")  pushChat({"", m.arg(0) + " joined", ChatLine::System});
			else if (c == "PART")  pushChat({"", m.arg(0) + " left", ChatLine::System});
			// USERCOUNT and others: ignored (the roster already conveys presence).
		};
		return cb;
	}

	// ---- Mode-agnostic helpers (dispatch on `mode`) ----

	// Tear down the JOIN session: stop/join the protocol client and clear all jam state.
	// Shared by the explicit stop (stopAll) and the server-drop path (handleServerDrop).
	void teardownJoin() {
		njclient.stop();
		joined = false;
		jamBpm.store(0, std::memory_order_relaxed);
		jamBpi.store(0, std::memory_order_relaxed);
		// Disarm transmit capture: a room switch never lets a widget step() observe the
		// joined=false gap (stopAll+joinStart run in one click), so without this txArmed
		// stays true from the old session and the next room's uploads start mid-interval
		// instead of on a downbeat — other clients hear us out of sync.
		txArmed.store(false, std::memory_order_relaxed);
		txNudge.store(false, std::memory_order_relaxed); // nudge is per-session
		std::lock_guard<std::mutex> lock(rosterMutex);
		roster.clear();
	}

	// Stop whatever is currently playing.
	void stopAll() {
		disconnectNote.clear(); // explicit user stop — nothing to report
		listenAltUrl.clear();
		if (listening) { stream.stop(); listening = false; }
		if (joined)
			teardownJoin();
	}

	// UI thread. The JOIN bg thread exited on its own while we still believe we're joined —
	// i.e. the server dropped us (kick / shutdown / network loss) or auth was rejected. Tear
	// down the dead session (so the panel returns to the connect view) and record a reason to
	// show on the status bar. Prefer the server's own words (the kick line) over the generic
	// terminal message.
	void handleServerDrop() {
		std::string reason;
		{
			std::lock_guard<std::mutex> lk(dropMutex);
			reason = !lastServerMsg.empty() ? lastServerMsg
			       : !termMsg.empty()       ? termMsg
			                                : std::string("Disconnected by server");
		}
		disconnectNote = reason;
		teardownJoin(); // njclient.stop() just joins the already-exited thread
	}

	// UI thread. The LISTEN (Icecast) stream hit a terminal error while we still believe we're
	// listening. The common case: the room's listen mount is unpublished (HTTP 404) — people can
	// be jamming on the NINJAM port while no source feeds Icecast, so there is simply nothing to
	// play. Without this the socket connected (no connect-failure logged), the decoder produced
	// zero frames, and the LED sat amber ("connecting") forever with no reason shown. Reconcile
	// like handleServerDrop(): read the reason, tear the dead stream down (so the panel returns
	// to the room browser and the LED leaves amber → red), and post the reason to the status bar.
	bool listenFailed() {
		return listening && stream.getState() == akaudio::StreamClient::State::Error;
	}
	void handleListenError() {
		std::string why = stream.getStatusText(); // read before stop() (which resets status)
		stream.stop();
		listening = false;
		// One-shot fallback: if the primary mount was unreachable but the room also
		// offers an https mount, try that before surfacing failure. Skip it on a 404 —
		// an empty mount means nobody is feeding Icecast (jam happening on the NINJAM
		// port only), and the alternate mount serves the same missing source.
		if (!listenAltUrl.empty() && why.find("404") == std::string::npos) {
			url = listenAltUrl;
			listenAltUrl.clear();
			listen();
			return;
		}
		listenAltUrl.clear();
		disconnectNote = why.find("404") != std::string::npos
			? std::string("Room's listen stream is offline")
			: why.empty() ? std::string("Stream unavailable") : why;
	}

	// ---- Per-room actions (the loudspeaker / enter icons on each row) ----
	void startListen(const akaudio::Room& room) {
		stopAll();
		mode = MODE_LISTEN;
		url = room.playUrl();
		// If the room also advertises an https mount that differs from the one we're
		// about to try, keep it as a fallback: the preferred plain-http mount is often
		// on a non-standard port that a firewall drops while :443 stays reachable.
		listenAltUrl = (!room.sslStream.empty() && room.sslStream != url) ? room.sslStream : "";
		roomLabel = room.name.empty() ? room.host : room.name;
		listen();
	}
	void startJoin(const akaudio::Room& room) {
		stopAll();
		mode = MODE_JOIN;
		joinHost = room.host;
		joinPort = room.port;
		roomLabel = room.name.empty() ? room.host : room.name;
		// Joining a jam means playing in it: auto-enable TX on every *explicit* join
		// (here and joinManual) so the user isn't silent because they forgot the toggle.
		// The patch-load rejoin (dataFromJson) deliberately does NOT do this.
		transmitting = true;
		joinStart();
	}
	// Direct join to a typed server (private/registered), with the credentials from
	// the panel fields. Credentials are assigned only after stopAll(), so the old
	// session (whose net thread may still be delivering callbacks) is fully joined
	// before any of this state changes.
	void joinManual(const std::string& host, int port, const std::string& user, const std::string& pass) {
		if (host.empty())
			return;
		stopAll();
		mode = MODE_JOIN;
		joinUser = user;
		joinPass = pass;
		joinHost = host;
		joinPort = port > 0 ? port : 2049;
		roomLabel = joinHost;
		transmitting = true; // explicit join → auto-enable TX (see startJoin)
		joinStart();
	}

	// ---- LISTEN (Icecast) ----
	void listen() {
		if (url.empty())
			return;
		disconnectNote.clear(); // a fresh source supersedes any prior drop reason
		stream.start(url);
		listening = true;
	}

	// ---- JOIN (protocol) ----
	void joinStart() {
		if (joinHost.empty())
			return;
		disconnectNote.clear(); // a fresh attempt supersedes any prior drop reason
		{
			std::lock_guard<std::mutex> lk(dropMutex);
			lastServerMsg.clear();
			termMsg.clear();
		}
		{
			std::lock_guard<std::mutex> lock(rosterMutex);
			roster.clear();
		}
		{
			std::lock_guard<std::mutex> lock(chatMutex);
			chatLog.clear();
			chatGen.fetch_add(1, std::memory_order_release);
		}
		jamBpm.store(0, std::memory_order_relaxed);
		jamBpi.store(0, std::memory_order_relaxed);
		njclient.setSampleRate(APP->engine->getSampleRate());
		resyncBeat.store(true, std::memory_order_release);
		njclient.start(joinHost, joinPort, joinUser, joinPass, jamCallbacks());
		joined = true;
		std::string hp = joinHost + ":" + std::to_string(joinPort);
		serverCreds[hp] = { joinUser, joinPass };
		addServerHistory(hp);
		saveGlobal(); // remember this server + its credentials for the next fresh module
	}

	// Declare/update our broadcast channels to match the TX toggle + connected poly input.
	// Called from the widget step() (UI thread); does no per-sample / audio-thread work.
	int txDeclaredVoice = -1; // voice flag of the last declaration (UI thread only)
	void syncTransmit() {
		int desired = 0;
		if (transmitting && joined && inputs[LEFT_INPUT].isConnected()) {
			int nin = inputs[LEFT_INPUT].getChannels();
			if (nin < 1) nin = 1;
			desired = std::min(nin, akaudio::nj::NjAudio::MAX_TX);
		}
		bool voice = txVoice.load(std::memory_order_relaxed);
		if (desired != txDeclared || (int) voice != txDeclaredVoice) {
			std::vector<std::string> names;
			for (int i = 0; i < desired; i++)
				names.push_back((voice ? "voice" : "ch") + std::to_string(i + 1));
			njclient.setTransmit(names, txQuality, voice);
			txDeclared = desired;
			txDeclaredVoice = (int) voice;
			if (desired == 0) txArmed.store(false, std::memory_order_relaxed);
		}
	}

	void addServerHistory(const std::string& hp) {
		if (hp.empty())
			return;
		auto it = std::find(serverHistory.begin(), serverHistory.end(), hp);
		if (it != serverHistory.end())
			serverHistory.erase(it);
		serverHistory.insert(serverHistory.begin(), hp);
		if (serverHistory.size() > 12) {
			for (size_t i = 12; i < serverHistory.size(); i++)
				serverCreds.erase(serverHistory[i]); // drop credentials for evicted servers
			serverHistory.resize(12);
		}
	}

	bool isActive() const { return listening || joined; }

	bool isListeningTo(const akaudio::Room& room) const {
		// Match either mount: after a fallback the active url is the room's ssl_stream.
		return listening && (url == room.playUrl() ||
		                     (!room.sslStream.empty() && url == room.sslStream));
	}
	bool isJoinedTo(const akaudio::Room& room) const {
		return joined && joinHost == room.host && joinPort == room.port;
	}
	void toggleListenRoom(const akaudio::Room& room) {
		if (isListeningTo(room)) stopAll();
		else startListen(room);
	}
	void toggleJoinRoom(const akaudio::Room& room) {
		if (isJoinedTo(room)) stopAll();
		else startJoin(room);
	}

	static bool roomCanListen(const akaudio::Room& room) { return room.playable(); }
	static bool roomCanJoin(const akaudio::Room& room) { return !room.host.empty() && room.port > 0; }

	// LED: green when the active source is live, amber while connecting.
	bool ledLive() const {
		if (joined) return njclient.state() == akaudio::nj::NjClient::State::Connected;
		if (listening) return stream.getState() == akaudio::StreamClient::State::Playing;
		return false;
	}
	bool ledPending() const {
		if (joined) return njclient.state() != akaudio::nj::NjClient::State::Connected;
		if (listening) return stream.getState() != akaudio::StreamClient::State::Playing;
		return false;
	}

	// One-line jam status for the panel when joined via protocol (empty otherwise).
	std::string jamStatusText() {
		if (!joined)
			return "";
		const char* sn = akaudio::nj::stateName(njclient.state());
		int bpm = jamBpm.load(std::memory_order_relaxed);
		int bpi = jamBpi.load(std::memory_order_relaxed);
		size_t n;
		{ std::lock_guard<std::mutex> lock(rosterMutex); n = roster.size(); }
		n += 1; // the roster lists only remote players; count ourselves too
		if (bpm > 0) {
			// Join-gap countdown: until the room's audio actually reaches the output
			// (preview or chained interval), estimate when it will — senders start the
			// first interval we can decode at the next boundary, and the preview plays
			// it ~a second in. Only when someone's actually there to hear.
			if (n > 1 && bpi > 0 && !njclient.audioStarted()) {
				float secs = (1.f - jamPhase.load(std::memory_order_relaxed))
					* (float) bpi * 60.f / (float) bpm + 1.f;
				return string::f("%s \xc2\xb7 %d BPM \xc2\xb7 %d BPI \xc2\xb7 audio in ~%ds",
					sn, bpm, bpi, (int) std::ceil(secs));
			}
			return string::f("%s \xc2\xb7 %d BPM \xc2\xb7 %d BPI \xc2\xb7 %d here", sn, bpm, bpi, (int) n);
		}
		return std::string(sn) + "\xe2\x80\xa6";
	}

	// Comma-joined list of players currently in the room (UI thread).
	std::string rosterText() {
		std::lock_guard<std::mutex> lock(rosterMutex);
		std::string s;
		for (size_t i = 0; i < roster.size(); i++) {
			if (i) s += ", ";
			s += roster[i];
		}
		return s;
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		stream.setSampleRate(e.sampleRate);
		njclient.setSampleRate(e.sampleRate);
	}

	void process(const ProcessArgs& args) override {
		// Audio thread: read the UI-owned flags once, relaxed (they're mode
		// switches, not synchronization points).
		const int modeL = mode.load(std::memory_order_relaxed);
		const bool joinedL = joined.load(std::memory_order_relaxed);
		const bool transmittingL = transmitting.load(std::memory_order_relaxed);
		const bool txVoiceL = txVoice.load(std::memory_order_relaxed);
		const int txDeclaredL = txDeclared.load(std::memory_order_relaxed);

		float l = 0.f, r = 0.f; // main mix (= sum of players in JOIN, or the Icecast stream)
		if (modeL == MODE_JOIN) {
			// Pull one wide frame (per-player stereo); fan out to the poly bundle and sum
			// to the main mix. Audio in Rack is ±5V line level.
			float frame[akaudio::nj::RING_CH];
			bool got = njclient.pullFrame(frame);
			int np = njclient.polyChannels();
			outputs[POLY_L_OUTPUT].setChannels(np);
			outputs[POLY_R_OUTPUT].setChannels(np);
			for (int i = 0; i < akaudio::nj::MAX_PLAYERS; i++) {
				float pl = got ? frame[i * 2] : 0.f;
				float pr = got ? frame[i * 2 + 1] : 0.f;
				if (i < np) {
					outputs[POLY_L_OUTPUT].setVoltage(pl * 5.f, i);
					outputs[POLY_R_OUTPUT].setVoltage(pr * 5.f, i);
				}
				l += pl;
				r += pr;
			}
		} else {
			outputs[POLY_L_OUTPUT].setChannels(0);
			outputs[POLY_R_OUTPUT].setChannels(0);
			stream.pull(l, r); // on underrun pull() leaves them untouched → they stay 0
		}

		// ---- Beat clock + metronome (only when joined to a tempo) ----
		int bpmL = jamBpm.load(std::memory_order_relaxed);
		int bpiL = jamBpi.load(std::memory_order_relaxed);
		if (args.sampleRate != cachedRate) {
			cachedRate = args.sampleRate;
			peakDecay = std::exp(-args.sampleTime / 0.15f);
			spbBpm = 0; // force samples-per-beat recompute
		}
		float click = 0.f, clockG = 0.f, resetG = 0.f, runG = 0.f, phaseV = 0.f;
		if (bpmL > 0 && bpiL > 0) {
			// Cheap relaxed load first; the RMW runs only on the rare resync frame.
			if (resyncBeat.load(std::memory_order_relaxed)
			        && resyncBeat.exchange(false, std::memory_order_acq_rel)) {
				beatPhase = 0.0;
				beatIndex = 0;
				currentBeat.store(0, std::memory_order_relaxed);
				clickEnv = 0.f;
				// The grid moved (join / server tempo change): disarm capture so the
				// next beat boundary re-arms it against the NEW grid with a fresh
				// silence prefill (txLoop closes the in-flight interval on re-arm).
				txArmed.store(false, std::memory_order_relaxed);
			}
			if (bpmL != spbBpm) {
				spbBpm = bpmL;
				spb = 60.0 * args.sampleRate / (double) bpmL; // samples per beat
			}
			beatPhase += 1.0;
			if (beatPhase >= spb) {
				beatPhase -= spb;
				beatIndex = (beatIndex + 1) % bpiL;
				currentBeat.store(beatIndex, std::memory_order_relaxed);
				// Arm transmit capture at the NEXT BEAT after TX comes on (≤1 beat of
				// wait), not the next interval downbeat (≤1 whole interval — half a
				// minute in a 32-BPI room). armTransmit tells the TX thread how much
				// of the current interval already elapsed; it leads the upload with
				// that much silence, so the interval grid stays downbeat-aligned.
				// (Voice mode skips arming entirely — capture starts immediately.)
				if (transmittingL && joinedL && txDeclaredL > 0 && !txVoiceL
				        && !txArmed.load(std::memory_order_relaxed)) {
					txArmed.store(true, std::memory_order_relaxed);
					njclient.armTransmit(beatIndex, bpiL);
				}
				clickEnv = 1.f; // arm a click; accent the downbeat
				clickPhase = 0.f;
				clickFreq = (beatIndex == 0) ? 1760.f : 880.f;
				clickAmp = (beatIndex == 0) ? 0.9f : 0.55f;
			}
			if (currentBeat.load(std::memory_order_relaxed) < 0)
				currentBeat.store(beatIndex, std::memory_order_relaxed);
			// Always synthesize the click tone (accented downbeat: higher + louder). It
			// goes to the CLICK jack unconditionally; the metronome toggle only decides
			// whether it is also mixed into the MAIN output (done below).
			if (clickEnv > 0.f) {
				click = std::sin(2.f * (float) M_PI * clickPhase) * clickEnv * clickAmp;
				clickPhase += clickFreq * args.sampleTime;
				if (clickPhase >= 1.f) clickPhase -= 1.f;
				clickEnv -= args.sampleTime / 0.035f; // ~35 ms decay
				if (clickEnv < 0.f) clickEnv = 0.f;
			}
			// CV sync outs: CLOCK = 50%-duty gate, `clockPpqn` pulses per beat; RESET = a
			// pulse only on the interval downbeat; RUN high while playing; PHASE = 0-10V
			// ramp across the interval.
			float beatFrac = (float) (beatPhase / spb);                 // 0..1 within beat
			float intervalPhase = ((float) beatIndex + beatFrac) / (float) bpiL; // 0..1
			int ppqn = clockPpqn.load(std::memory_order_relaxed);
			if (ppqn < 1) ppqn = 1;
			float subFrac = beatFrac * (float) ppqn;
			subFrac -= (float) (int) subFrac;                           // 0..1 within each sub-pulse
			clockG = (subFrac < 0.5f) ? 10.f : 0.f;
			resetG = (beatIndex == 0 && beatFrac < 0.5f) ? 10.f : 0.f;
			runG = 10.f;
			phaseV = intervalPhase * 10.f;
			jamPhase.store(intervalPhase, std::memory_order_relaxed);
		} else {
			currentBeat.store(-1, std::memory_order_relaxed);
			jamPhase.store(0.f, std::memory_order_relaxed);
			beatPhase = 0.0;
			beatIndex = 0;
			clickEnv = 0.f;
		}
		// ---- Transmit capture: feed input frames once armed at a beat boundary ----
		// (voice mode: immediately — it has no grid to align to)
		if (transmittingL && joinedL && txDeclaredL > 0
		        && (txVoiceL || txArmed.load(std::memory_order_relaxed))
		        && inputs[LEFT_INPUT].isConnected()) {
			bool rConn = inputs[RIGHT_INPUT].isConnected();
			for (int ch = 0; ch < txDeclaredL; ch++) {
				float il = inputs[LEFT_INPUT].getPolyVoltage(ch) * 0.2f;  // ±5V -> ±1
				float ir = rConn ? inputs[RIGHT_INPUT].getPolyVoltage(ch) * 0.2f : il;
				njclient.captureFrame(ch, il, ir);
			}
		}

		// Main mix: fold the metronome click in when the toggle is on, so it is audible on
		// MAIN without needing to patch the CLICK jack.
		if (clickEnabled.load(std::memory_order_relaxed))
			{ l += click; r += click; }
		outputs[LEFT_OUTPUT].setVoltage(l * 5.f);
		outputs[RIGHT_OUTPUT].setVoltage(r * 5.f);

		// Peak meter: fast attack, ~150 ms exponential release.
		float amp = std::max(std::fabs(l), std::fabs(r));
		float p = peak.load(std::memory_order_relaxed);
		p = amp > p ? amp : p * peakDecay;
		peak.store(p, std::memory_order_relaxed);

		outputs[CLICK_OUTPUT].setVoltage(click * 5.f);
		outputs[CLOCK_OUTPUT].setVoltage(clockG);
		outputs[RESET_OUTPUT].setVoltage(resetG);
		outputs[RUN_OUTPUT].setVoltage(runG);
		outputs[PHASE_OUTPUT].setVoltage(phaseV);
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "mode", json_integer(mode));
		json_object_set_new(root, "url", json_string(url.c_str()));
		// roomLabel echoes the private join host in JOIN mode — keep it out of the patch
		// (re-derived on load); persist it only for LISTEN, where it's a public room name.
		if (mode == MODE_LISTEN)
			json_object_set_new(root, "roomLabel", json_string(roomLabel.c_str()));
		json_object_set_new(root, "listening", json_boolean(listening));
		// NOTE: join server/username/password are deliberately NOT persisted here — they
		// live only in the global file (saveGlobal), so a shared patch leaks no credentials.
		json_object_set_new(root, "joined", json_boolean(joined));
		json_object_set_new(root, "clickEnabled", json_boolean(clickEnabled));
		json_object_set_new(root, "clockPpqn", json_integer(clockPpqn.load(std::memory_order_relaxed)));
		// NOTE: `transmitting` is deliberately NOT persisted — broadcasting your live audio
		// input must be a fresh, explicit per-session choice, never something a loaded patch
		// silently resumes. txQuality/txVoice (settings, not triggers) are fine to keep.
		json_object_set_new(root, "txQuality", json_real(txQuality));
		json_object_set_new(root, "txVoice", json_boolean(txVoice));
		return root;
	}

	void dataFromJson(json_t* root) override {
		// Loading a preset/patch onto a live module: tear down whatever is currently
		// playing FIRST, before overwriting the mode/joined/listening flags below.
		// Otherwise a running LISTEN stream or JOIN session is orphaned — its bg
		// thread keeps going, unreachable because stopAll() now sees the new flags.
		// (UI/main thread here, per the contract; stopAll joins the bg threads.)
		stopAll();

		if (json_t* j = json_object_get(root, "mode"))
			mode = (int) json_integer_value(j);
		url = jsonStr(json_object_get(root, "url"), url);
		roomLabel = jsonStr(json_object_get(root, "roomLabel"), roomLabel);
		if (json_t* j = json_object_get(root, "listening"))
			listening = json_boolean_value(j);
		// Join server/creds are not in the patch; they came from loadGlobal() in the ctor.
		// Re-derive the JOIN panel label from the prefilled host so the panel isn't blank.
		if (json_t* j = json_object_get(root, "joined"))
			joined = json_boolean_value(j);
		if (mode == MODE_JOIN && !joinHost.empty())
			roomLabel = joinHost;
		if (json_t* j = json_object_get(root, "clickEnabled"))
			clickEnabled = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "clockPpqn"))
			clockPpqn.store((int) json_integer_value(j), std::memory_order_relaxed);
		// Transmit always starts OFF on load (not persisted, above) — a loaded patch may
		// auto-rejoin a room but must never auto-broadcast the user's input.
		transmitting = false;
		if (json_t* j = json_object_get(root, "txQuality"))
			txQuality = (float) json_real_value(j);
		if (json_t* j = json_object_get(root, "txVoice"))
			txVoice = json_boolean_value(j);

		// Auto-resume on patch load (JOIN reconnects to the local default server; the
		// user's audio is NOT transmitted until they click TX or explicitly join a
		// room themselves — see transmitting above).
		if (mode == MODE_JOIN && joined) {
			joined = false; // joinStart() sets it
			txNudge = true; // rejoined silent → panel shows the START TRANSMITTING nudge
			joinStart();
		} else if (mode == MODE_LISTEN && listening) {
			listening = false; // listen() sets it
			listen();
		}
	}
};

// Parse "host[:port]" into host + port (default 2049).
// Shared connect action for the in-panel Direct Join card: pulls username/password from
// their fields and the host:port from the server field, then joins (private/registered
// servers that aren't in the public directory).
static void directJoin(Ninjam* module, ui::TextField* userF, ui::TextField* passF, ui::TextField* serverF) {
	if (!module || !serverF || serverF->text.empty())
		return;
	std::string host;
	int port;
	parseHostPort(serverF->text, host, port);
	// Credentials are handed to joinManual (assigned after it stops the old
	// session), not written to the module while a session may still be live.
	module->joinManual(host, port,
		userF ? userF->text : module->joinUser,
		passF ? passF->text : module->joinPass);
}

// Server "host:port" field — pressing Enter connects, same as the Join button.
// (TAB focus cycling comes free with ui::TextField's nextField/prevField.)
struct ServerField : ui::TextField {
	Ninjam* module = nullptr;
	ui::TextField* userField = nullptr;
	ui::TextField* passField = nullptr;
	void onAction(const ActionEvent& e) override {
		directJoin(module, userField, passField, this);
		ui::TextField::onAction(e);
	}
};

// ---- In-panel "deluxe" room browser -------------------------------------
//
// A scrollable, searchable list of public rooms drawn right on the panel. It
// reads the RoomDirectory cache (never the network) on the UI thread and only
// rebuilds its rows when the cache generation or the filter text changes.

// Fonts (FONT_BOLD/FONT_REG/FONT_MONO) + the cross-module colors and plate
// geometry live in Theme.hpp — shared with Radio so the panels stay in lockstep.

// Chat "console" palette: light monospace text on a dark recessed pane.
static const NVGcolor TH_CON_BG    = nvgRGB(0x16, 0x1a, 0x17); // console background (near-black)
static const NVGcolor TH_CON_TEXT  = nvgRGB(0xcf, 0xd6, 0xcd); // message text
static const NVGcolor TH_CON_DIM   = nvgRGB(0x6f, 0x79, 0x6f); // system lines / placeholder
static const NVGcolor TH_CON_NAME  = nvgRGB(0x5f, 0xc8, 0x86); // others' speaker name (green)
static const NVGcolor TH_CON_MINE  = nvgRGB(0x9b, 0xef, 0xbb); // our own speaker name (brighter green)

static const NVGcolor NINJAM_GREEN = nvgRGB(0x2a, 0xa8, 0x55); // accent (darker for light panel)

// ---- Light "Fundamental-style" theme ----
static const NVGcolor TH_TEXT     = nvgRGB(0x24, 0x27, 0x2b); // primary dark text
static const NVGcolor TH_TEXT_DIM = nvgRGB(0x5c, 0x61, 0x68); // secondary text
static const NVGcolor TH_CARD     = nvgRGB(0xd6, 0xd9, 0xdc); // card/well fill (vs panel)
static const NVGcolor TH_CARD_BD  = nvgRGBA(0x00, 0x00, 0x00, 0x26); // card/well border
static const NVGcolor TH_WELL     = nvgRGBA(0x00, 0x00, 0x00, 0x10); // recessed area fill
static const NVGcolor TH_IN_BD    = nvgRGB(0x3c, 0x6f, 0xc4); // input frame stroke (blue)
// (Output-plate black/label colors: AK_PLATE / AK_PLATE_TEXT in Theme.hpp.)

static std::string lower(std::string s) {
	for (char& c : s)
		c = (char) std::tolower((unsigned char) c);
	return s;
}

static bool roomMatches(const akaudio::Room& r, const std::string& f) {
	if (lower(r.name).find(f) != std::string::npos)
		return true;
	for (const std::string& u : r.users)
		if (lower(u).find(f) != std::string::npos)
			return true;
	return false;
}

// (drawTxt / textWidth live in Theme.hpp.)

// Break `s` into rows that each fit `maxW` px at the given font/size (word wrap, falling
// back to mid-word breaks for unbreakable runs — nanovg handles both). Used by the chat
// console so long messages flow across rows instead of being ellipsized.
static std::vector<std::string> wrapText(NVGcontext* vg, const char* fontRes, float size,
		float maxW, const std::string& s) {
	std::vector<std::string> out;
	std::shared_ptr<window::Font> font = akLoadFont(fontRes);
	if (!font || font->handle < 0 || maxW <= 0.f) {
		out.push_back(s);
		return out;
	}
	nvgFontFaceId(vg, font->handle);
	nvgFontSize(vg, size);
	const char* start = s.c_str();
	const char* end = start + s.size();
	NVGtextRow rows[4];
	int n;
	while (start < end && (n = nvgTextBreakLines(vg, start, end, maxW, rows, 4)) > 0) {
		for (int i = 0; i < n; i++)
			out.push_back(std::string(rows[i].start, rows[i].end - rows[i].start));
		start = rows[n - 1].next;
	}
	if (out.empty())
		out.push_back("");
	return out;
}

// Loudspeaker icon (listen), drawn as vector paths — the bundled fonts have no speaker
// glyph. Centered at (cx,cy); `s` is the nominal icon size.
static void drawSpeakerIcon(NVGcontext* vg, float cx, float cy, float s, NVGcolor col) {
	const float u = s * 0.5f;
	nvgBeginPath(vg); // body (rect) + cone (triangle) silhouette
	nvgMoveTo(vg, cx - 0.80f * u, cy - 0.28f * u);
	nvgLineTo(vg, cx - 0.30f * u, cy - 0.28f * u);
	nvgLineTo(vg, cx + 0.15f * u, cy - 0.70f * u);
	nvgLineTo(vg, cx + 0.15f * u, cy + 0.70f * u);
	nvgLineTo(vg, cx - 0.30f * u, cy + 0.28f * u);
	nvgLineTo(vg, cx - 0.80f * u, cy + 0.28f * u);
	nvgClosePath(vg);
	nvgFillColor(vg, col);
	nvgFill(vg);
	nvgStrokeColor(vg, col);
	nvgStrokeWidth(vg, std::max(1.f, s * 0.07f));
	for (int k = 1; k <= 2; k++) { // sound waves
		nvgBeginPath(vg);
		nvgArc(vg, cx + 0.15f * u, cy, u * (0.30f + 0.28f * k), -0.6f, 0.6f, NVG_CW);
		nvgStroke(vg);
	}
}

// "Enter the room" icon (join): a door frame on the right with an arrow going into it.
static void drawEnterIcon(NVGcontext* vg, float cx, float cy, float s, NVGcolor col) {
	const float u = s * 0.5f;
	nvgStrokeColor(vg, col);
	nvgStrokeWidth(vg, std::max(1.f, s * 0.08f));
	nvgLineCap(vg, NVG_ROUND);
	nvgLineJoin(vg, NVG_ROUND);
	nvgBeginPath(vg); // door frame (three sides, open toward the arrow)
	nvgMoveTo(vg, cx + 0.15f * u, cy - 0.78f * u);
	nvgLineTo(vg, cx + 0.78f * u, cy - 0.78f * u);
	nvgLineTo(vg, cx + 0.78f * u, cy + 0.78f * u);
	nvgLineTo(vg, cx + 0.15f * u, cy + 0.78f * u);
	nvgStroke(vg);
	nvgBeginPath(vg); // arrow shaft
	nvgMoveTo(vg, cx - 0.85f * u, cy);
	nvgLineTo(vg, cx + 0.35f * u, cy);
	nvgStroke(vg);
	nvgBeginPath(vg); // arrow head
	nvgMoveTo(vg, cx + 0.02f * u, cy - 0.32f * u);
	nvgLineTo(vg, cx + 0.38f * u, cy);
	nvgLineTo(vg, cx + 0.02f * u, cy + 0.32f * u);
	nvgStroke(vg);
}

// One room in the list. Two icons on the right: speaker = Listen (Icecast), door = Join
// (protocol). Each toggles independently; the row highlights when this room is active.
struct RoomRow : HoverButton {
	Ninjam* module = nullptr;
	akaudio::Room room;
	int hoveredIcon = 0; // 0 none, 1 listen, 2 join
	// Precomputed draw strings (a room is immutable once the row is built; don't
	// re-format them per frame).
	std::string statsText;
	std::string usersText;

	static constexpr float ICON = 15.f;
	float listenCx() const { return box.size.x - 36.f; }
	float joinCx() const { return box.size.x - 15.f; }
	static constexpr float ICON_CY = 13.f;

	void setRoom(const akaudio::Room& r) {
		room = r;
		std::string cap = r.userMax > 0 ? string::f("%d/%d", r.userCount, r.userMax)
		                                : string::f("%d", r.userCount);
		statsText = string::f("%d BPM \xc2\xb7 %d BPI \xc2\xb7 ", r.bpm, r.bpi) + cap + " here";
		usersText.clear();
		for (size_t i = 0; i < r.users.size(); i++) {
			if (i)
				usersText += ", ";
			usersText += r.users[i];
		}
	}

	// 1 = listen icon, 2 = join icon, 0 = neither, for a point in local coords.
	int iconAt(math::Vec p) const {
		if (std::fabs(p.y - ICON_CY) > 11.f) return 0;
		if (std::fabs(p.x - listenCx()) <= 11.f) return 1;
		if (std::fabs(p.x - joinCx()) <= 11.f) return 2;
		return 0;
	}

	void onLeave(const LeaveEvent& e) override {
		hoveredIcon = 0;
		HoverButton::onLeave(e);
	}
	void onHover(const HoverEvent& e) override {
		hoveredIcon = iconAt(e.pos);
		HoverButton::onHover(e);
	}
	void onPress(const ButtonEvent& e) override {
		if (module) {
			int which = iconAt(e.pos);
			if (which == 1 && Ninjam::roomCanListen(room)) {
				module->toggleListenRoom(room);
				e.consume(this);
				return;
			}
			if (which == 2 && Ninjam::roomCanJoin(room)) {
				module->toggleJoinRoom(room);
				e.consume(this);
				return;
			}
		}
		OpaqueWidget::onButton(e); // not on an icon: fall through (row itself is inert)
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		const float w = box.size.x, h = box.size.y, pad = 10.f;
		const bool listening = module && module->isListeningTo(room);
		const bool joined = module && module->isJoinedTo(room);
		const bool active = listening || joined;

		if (active || hovered) {
			nvgBeginPath(vg);
			nvgRoundedRect(vg, 2, 1, w - 4, h - 2, 3);
			nvgFillColor(vg, active ? nvgRGBA(0x2a, 0xa8, 0x55, 0x2e) : nvgRGBA(0x00, 0x00, 0x00, 0x0c));
			nvgFill(vg);
		}
		if (active) {
			nvgBeginPath(vg);
			nvgRoundedRect(vg, 2, 3, 3, h - 6, 1.5);
			nvgFillColor(vg, NINJAM_GREEN);
			nvgFill(vg);
		}

		// Name (clipped before the icons).
		drawTxt(vg, FONT_BOLD, pad, ICON_CY, 14.5f,
			active ? nvgRGB(0x1c, 0x7a, 0x3e) : TH_TEXT,
			room.name, NVG_ALIGN_LEFT, w - pad - 52);

		// Listen (speaker) + Join (enter) icons.
		const NVGcolor dim = nvgRGBA(0x00, 0x00, 0x00, 0x55);
		const NVGcolor off = nvgRGBA(0x00, 0x00, 0x00, 0x22);
		const NVGcolor bright = TH_TEXT;
		NVGcolor lc = !Ninjam::roomCanListen(room) ? off
		            : listening ? NINJAM_GREEN
		            : hoveredIcon == 1 ? bright : dim;
		NVGcolor jc = !Ninjam::roomCanJoin(room) ? off
		            : joined ? NINJAM_GREEN
		            : hoveredIcon == 2 ? bright : dim;
		drawSpeakerIcon(vg, listenCx(), ICON_CY, ICON, lc);
		drawEnterIcon(vg, joinCx(), ICON_CY, ICON, jc);

		// Stats line.
		drawTxt(vg, FONT_REG, pad, 26, 11.5f, TH_TEXT_DIM, statsText, NVG_ALIGN_LEFT, w - 2 * pad);

		// Players line (if anyone is in the room).
		if (!usersText.empty())
			drawTxt(vg, FONT_REG, pad, 39, 10.f, nvgRGB(0x3a, 0x86, 0x55), usersText, NVG_ALIGN_LEFT, w - 2 * pad);
	}
};

// The scrollable list. Rebuilds rows only when the cache or filter changes, and
// kicks a background refresh periodically while visible.
struct RoomBrowser : ui::ScrollWidget {
	Ninjam* module = nullptr;
	ui::TextField* search = nullptr;
	unsigned lastGen = (unsigned) -1;
	std::string lastFilter = std::string(1, '\x01'); // force the first build
	int refreshTimer = 0;

	void rebuild() {
		container->clearChildren();
		if (!module)
			return;
		const float w = box.size.x;
		const std::string filter = search ? lower(search->text) : "";
		std::vector<akaudio::Room> rooms = module->directory.rooms();
		float y = 0;
		for (const akaudio::Room& room : rooms) {
			if (!Ninjam::roomCanListen(room) && !Ninjam::roomCanJoin(room))
				continue; // unusable (no stream and no host/port)
			if (!filter.empty() && !roomMatches(room, filter))
				continue;
			RoomRow* row = new RoomRow;
			row->module = module;
			row->setRoom(room);
			row->box.pos = Vec(0, y);
			row->box.size = Vec(w, room.users.empty() ? 31.f : 45.f);
			container->addChild(row);
			y += row->box.size.y;
		}
		container->box.size = Vec(w, y);
	}

	// True once the room list has been loaded (or is loading) — i.e. the user has
	// asked for it at least once. Used to gate the periodic refresh so the FIRST
	// ninbot fetch is always user-initiated, never automatic.
	bool directoryEngaged() const {
		return module && (module->directory.generation() > 0 || module->directory.loading());
	}

	// User asked to see the room list (clicked it / focused search): kick the first
	// fetch if one hasn't happened yet. No-op once loaded — step() keeps it fresh.
	void requestLoad() {
		if (module && module->directory.generation() == 0 && !module->directory.loading())
			module->directory.refresh();
	}

	void onButton(const ButtonEvent& e) override {
		requestLoad(); // first interaction with the room list loads it
		ui::ScrollWidget::onButton(e);
	}

	void step() override {
		// Rack steps hidden widgets too; the browser is hidden while joined/listening,
		// so gate the background directory refresh (and the row rebuild) on visibility —
		// no point fetching ninbot every 30 s for a list nobody can see. Only keep an
		// already-engaged list fresh; the first fetch comes from requestLoad(), not here.
		if (module && visible) {
			if (directoryEngaged() && ++refreshTimer >= 60 * 30) { // ~30 s at 60 fps
				refreshTimer = 0;
				module->directory.refresh();
			}
			unsigned g = module->directory.generation();
			std::string f = search ? lower(search->text) : "";
			if (g != lastGen || f != lastFilter) {
				lastGen = g;
				lastFilter = f;
				rebuild();
				// A freshly filled list always presents from the top: rooms sort
				// busiest-first, so the bottom is the dead end — and a shrunken list
				// would otherwise leave the old scroll offset clamped down there.
				offset = math::Vec(0, 0);
			}
		}
		ui::ScrollWidget::step();
	}

	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 4);
		nvgFillColor(args.vg, TH_WELL);
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, TH_CARD_BD);
		nvgStroke(args.vg);
		ui::ScrollWidget::draw(args);
		if (container->children.empty()) {
			std::string msg;
			if (module && module->directory.loading())
				msg = "Loading rooms\xe2\x80\xa6";
			else if (module && module->directory.generation() == 0)
				msg = "Click or Refresh to load rooms"; // not fetched yet (user-initiated)
			else
				msg = "No rooms \xe2\x80\x94 try Refresh";
			drawTxt(args.vg, FONT_REG, box.size.x / 2, box.size.y / 2, 13.f,
				TH_TEXT_DIM, msg, NVG_ALIGN_CENTER);
		}
	}
};

// Compact search box.
struct SearchField : ui::TextField {
	Ninjam* module = nullptr;
	SearchField() {
		placeholder = "Filter rooms or players\xe2\x80\xa6";
	}
	void onSelect(const SelectEvent& e) override {
		// Focusing the filter is a user-initiated request for the list — load it if the
		// directory hasn't been fetched yet (keeps the first ninbot contact user-driven).
		if (module && module->directory.generation() == 0 && !module->directory.loading())
			module->directory.refresh();
		ui::TextField::onSelect(e);
	}
};

// Chat input — Enter sends the line to the room, then clears it. The server echoes
// our line back (with our name), so the log shows it via the normal onChat path.
// Drawn frameless (no blendish box): a monospace prompt + text + caret on the dark
// chat console, so it reads as the bottom line of a TTY rather than a boxed widget.
struct ChatField : ui::TextField {
	Ninjam* module = nullptr;
	ChatField() { placeholder = "type a message, /bpi 16, !vote bpm 120\xe2\x80\xa6"; }
	void onAction(const ActionEvent& e) override {
		if (module && !text.empty()) {
			module->sendChatLine(text);
			setText("");
		}
		e.consume(this);
	}
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		nvgScissor(vg, RECT_ARGS(args.clipBox));
		const float size = 12.5f, cy = box.size.y / 2.f;
		const bool focused = (this == APP->event->getSelectedWidget());
		// Prompt.
		const char* prompt = "\xe2\x80\xba "; // "› "
		drawTxt(vg, FONT_MONO, 3.f, cy, size, TH_CON_NAME, prompt, NVG_ALIGN_LEFT);
		const float tx = 3.f + textWidth(vg, FONT_MONO, size, prompt);
		if (text.empty() && !focused) {
			drawTxt(vg, FONT_MONO, tx, cy, size, TH_CON_DIM, placeholder, NVG_ALIGN_LEFT,
				box.size.x - tx - 3.f);
		} else {
			// Selection highlight.
			if (focused && cursor != selection) {
				int a = std::min(cursor, selection), b = std::max(cursor, selection);
				float xa = tx + textWidth(vg, FONT_MONO, size, text.substr(0, a));
				float xb = tx + textWidth(vg, FONT_MONO, size, text.substr(0, b));
				nvgBeginPath(vg);
				nvgRect(vg, xa, cy - size * 0.7f, xb - xa, size * 1.4f);
				nvgFillColor(vg, nvgRGBA(0x2a, 0xc8, 0x66, 0x55));
				nvgFill(vg);
			}
			drawTxt(vg, FONT_MONO, tx, cy, size, TH_CON_TEXT, text, NVG_ALIGN_LEFT);
			if (focused) {
				float cx = tx + textWidth(vg, FONT_MONO, size, text.substr(0, cursor));
				nvgBeginPath(vg);
				nvgRect(vg, cx, cy - size * 0.62f, 1.3f, size * 1.24f);
				nvgFillColor(vg, TH_CON_NAME);
				nvgFill(vg);
			}
		}
		nvgResetScissor(vg);
	}
};

// Clickable refresh control (re-fetches the directory in the background).
struct RefreshButton : HoverButton {
	Ninjam* module = nullptr;

	void onPress(const ButtonEvent& e) override {
		if (module)
			module->directory.refresh();
		e.consume(this);
	}
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, 3);
		nvgFillColor(vg, hovered ? nvgRGBA(0x00, 0x00, 0x00, 0x18) : TH_WELL);
		nvgFill(vg);
		nvgStrokeColor(vg, TH_CARD_BD);
		nvgStroke(vg);
		bool loading = module && module->directory.loading();
		drawTxt(vg, FONT_REG, box.size.x / 2, box.size.y / 2, 16.f,
			loading ? NINJAM_GREEN : TH_TEXT, "\xe2\x86\xbb", NVG_ALIGN_CENTER); // ↻
	}
};

// Transmit enable: lights green when broadcasting our input channels.
struct TxToggle : HoverButton {
	Ninjam* module = nullptr;
	void onPress(const ButtonEvent& e) override {
		if (module) {
			module->transmitting = !module->transmitting;
			module->txNudge = false; // any explicit TX decision retires the nudge
		}
		e.consume(this);
	}
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		bool on = module && module->transmitting;
		const float cx = box.size.x / 2, cy = box.size.y / 2, r = box.size.x / 2 - 1.f;
		// Soft glow when transmitting.
		if (on) {
			nvgBeginPath(vg);
			nvgCircle(vg, cx, cy, r + 3.f);
			NVGpaint glow = nvgRadialGradient(vg, cx, cy, r, r + 4.f,
				nvgRGBA(0x2e, 0xd1, 0x6b, 0xb0), nvgRGBA(0x2e, 0xd1, 0x6b, 0x00));
			nvgFillPaint(vg, glow);
			nvgFill(vg);
		}
		nvgBeginPath(vg);
		nvgCircle(vg, cx, cy, r);
		nvgFillColor(vg, on ? (hovered ? nvgRGB(0x4a, 0xe0, 0x82) : NINJAM_GREEN)
		                    : (hovered ? nvgRGBA(0x00, 0x00, 0x00, 0x20) : TH_WELL));
		nvgFill(vg);
		nvgStrokeColor(vg, TH_CARD_BD);
		nvgStrokeWidth(vg, 1.f);
		nvgStroke(vg);
	}
};

// Metronome icon (vector) — trapezoid body + pendulum rod + weight.
static void drawMetronomeIcon(NVGcontext* vg, float cx, float cy, float s, NVGcolor col) {
	const float u = s * 0.5f;
	nvgStrokeColor(vg, col);
	nvgStrokeWidth(vg, std::max(1.f, s * 0.08f));
	nvgLineJoin(vg, NVG_ROUND);
	nvgBeginPath(vg); // body (trapezoid, wider at base)
	nvgMoveTo(vg, cx - 0.50f * u, cy + 0.85f * u);
	nvgLineTo(vg, cx + 0.50f * u, cy + 0.85f * u);
	nvgLineTo(vg, cx + 0.24f * u, cy - 0.85f * u);
	nvgLineTo(vg, cx - 0.24f * u, cy - 0.85f * u);
	nvgClosePath(vg);
	nvgStroke(vg);
	nvgBeginPath(vg); // pendulum rod
	nvgMoveTo(vg, cx - 0.02f * u, cy + 0.55f * u);
	nvgLineTo(vg, cx + 0.26f * u, cy - 0.65f * u);
	nvgStroke(vg);
	nvgBeginPath(vg); // weight on the rod
	nvgRect(vg, cx + 0.02f * u, cy - 0.18f * u, 0.20f * u, 0.18f * u);
	nvgFillColor(vg, col);
	nvgFill(vg);
}

// Metronome click toggle (lights green when on).
struct MetronomeToggle : HoverButton {
	Ninjam* module = nullptr;
	void onPress(const ButtonEvent& e) override {
		if (module)
			module->clickEnabled = !module->clickEnabled;
		e.consume(this);
	}
	void draw(const DrawArgs& args) override {
		bool on = module && module->clickEnabled;
		NVGcolor col = on ? NINJAM_GREEN
		             : hovered ? TH_TEXT : nvgRGBA(0x00, 0x00, 0x00, 0x55);
		drawMetronomeIcon(args.vg, box.size.x / 2, box.size.y / 2, std::min(box.size.x, box.size.y), col);
	}
};

// Top transport block: connection/tempo status (or directory status), with a row of
// per-beat ticks under it when joined (elapsed lit, current brightest, downbeat accented).
// Top status bar (always visible): connection/tempo line; metronome toggle + LED sit on
// its right (separate widgets). Single line, vertically centered.
struct TransportBlock : Widget {
	Ninjam* module = nullptr;
	void draw(const DrawArgs& args) override {
		if (!module)
			return;
		NVGcontext* vg = args.vg;
		const float w = box.size.x, h = box.size.y;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, w, h, 4);
		nvgFillColor(vg, TH_WELL);
		nvgFill(vg);
		nvgStrokeColor(vg, TH_CARD_BD);
		nvgStroke(vg);
		std::string jam = module->jamStatusText();
		const bool joined = !jam.empty();
		// When not joined, an unexpected drop reason (kick / server loss) takes over the line in
		// amber until the next connect attempt; otherwise the live room-directory status shows.
		const bool dropped = !joined && !module->disconnectNote.empty();
		NVGcolor col = joined  ? nvgRGB(0x1c, 0x7a, 0x3e)
		             : dropped ? nvgRGB(0xc0, 0x7a, 0x16)
		                       : TH_TEXT_DIM;
		drawTxt(vg, FONT_BOLD, 8, h / 2, 11.5f, col,
			joined ? jam : dropped ? module->disconnectNote : module->directory.status(),
			NVG_ALIGN_LEFT, w - 8 - 44);
	}
};

// The connected "jam view" (replaces the server-selection UI once joined): big beat-tick
// row, an interval progress bar, and the roster of who's in the room.
struct JamView : Widget {
	Ninjam* module = nullptr;

	// Chat scrollback. `scrollUp` = wrapped rows lifted above the newest (0 = pinned to the
	// bottom, auto-tailing). `lastTotal` lets us hold the viewport on the same messages when
	// new lines arrive while scrolled up. draw() clamps both against what currently fits.
	int scrollUp = 0;
	int lastTotal = -1;

	// One physical line of the console. `name` (when set) prints at tpad in nameCol; the
	// body prints at bodyX in bodyCol. Continuation rows of a wrapped message carry no
	// name and hang-indent their body under where the first row's text began.
	struct Row {
		float size;
		std::string name; NVGcolor nameCol;
		float bodyX; NVGcolor bodyCol; std::string body;
	};
	// Wrapped-row cache: snapshotting + word-wrapping the whole log (up to 200 lines,
	// to draw ~15) is far too much for every UI frame — rebuild only when the log
	// (chatGen) or the pane width changes.
	std::vector<Row> rows;
	unsigned rowsGen = (unsigned) -1;
	float rowsClip = -1.f;

	// Mouse wheel scrolls the chat log. Wheel up (scrollDelta.y > 0, same sign Rack's
	// ScrollWidget uses) reveals older lines; down returns toward the newest.
	void onHoverScroll(const HoverScrollEvent& e) override {
		if (e.scrollDelta.y > 0.f)
			scrollUp += 3;
		else if (e.scrollDelta.y < 0.f)
			scrollUp -= 3;
		if (scrollUp < 0)
			scrollUp = 0;
		e.consume(this);
	}

	void draw(const DrawArgs& args) override {
		if (!module)
			return;
		NVGcontext* vg = args.vg;
		const float w = box.size.x, pad = 12.f, avail = w - 2 * pad;
		int bpi = module->jamBpi.load(std::memory_order_relaxed);
		int cur = module->currentBeat.load(std::memory_order_relaxed);

		// Beat ticks — one tick per beat in the bar, reflowed into as many rows as it takes
		// to keep each tick legible. BPI is server-driven and changes live when any admin
		// votes/commands a new tempo, so the whole block re-lays-out (and the chat below
		// shifts) on the fly. `tickBottom` is where the row(s) end.
		float tickBottom = 4.f;
		if (bpi > 0) {
			const float ty = 4.f, gap = 3.f, rowGap = 3.f, minTW = 10.f;
			// How many ticks fit per row at the legible minimum, then balance across rows.
			int perRow = std::max(1, (int) std::floor((avail + gap) / (minTW + gap)));
			int rows = std::min(4, (bpi + perRow - 1) / perRow);
			perRow = (bpi + rows - 1) / rows;
			float tw = (avail - (perRow - 1) * gap) / perRow;
			if (tw < 1.f) tw = 1.f;
			// Tick height: full 20 px for a single row, shrinking as rows stack (cap block ~40 px).
			float th = std::min(20.f, (40.f - (rows - 1) * rowGap) / rows);
			for (int b = 0; b < bpi; b++) {
				int row = b / perRow, col = b % perRow;
				float x = pad + col * (tw + gap);
				float y = ty + row * (th + rowGap);
				NVGcolor c = (b == cur) ? nvgRGB(0x2a, 0xc8, 0x66)
				           : (cur >= 0 && b < cur) ? NINJAM_GREEN
				           : (b == 0) ? nvgRGB(0xc8, 0x9a, 0x3a)
				           : nvgRGBA(0x00, 0x00, 0x00, 0x1a);
				nvgBeginPath(vg);
				nvgRoundedRect(vg, x, y, tw, th, 2.f);
				nvgFillColor(vg, c);
				nvgFill(vg);
			}
			tickBottom = ty + rows * th + (rows - 1) * rowGap;
		}

		// Info line: beat counter (left) + who's here (right), just under the ticks.
		const float infoY = tickBottom + 12.f;
		if (cur >= 0)
			drawTxt(vg, FONT_REG, pad, infoY, 10.5f, TH_TEXT_DIM,
				string::f("%d BPM \xc2\xb7 beat %d / %d",
					module->jamBpm.load(std::memory_order_relaxed), cur + 1, bpi),
				NVG_ALIGN_LEFT);
		std::string who = module->rosterText();
		std::string here = who.empty() ? std::string("just you")
		                               : (std::string("here: you, ") + who);
		drawTxt(vg, FONT_REG, w - pad, infoY, 10.5f, nvgRGB(0x1c, 0x7a, 0x3e), here,
			NVG_ALIGN_RIGHT, avail * 0.62f);

		// Chat console — a recessed dark TTY pane holding the message log and, along its
		// bottom, the frameless input line (the ChatField sibling overlays that strip).
		const float conTop = infoY + 10.f, conBot = box.size.y;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, pad, conTop, avail, conBot - conTop, 4.f);
		nvgFillColor(vg, TH_CON_BG);
		nvgFill(vg);

		const float inset = 6.f;
		const float inputTop = conBot - 24.f;     // input strip (ChatField sits here)
		const float logTop = conTop + inset, logBot = inputTop - 4.f;
		const float lineH = 13.f;

		// Faint rule between the log and the input line.
		nvgBeginPath(vg);
		nvgMoveTo(vg, pad + inset, inputTop);
		nvgLineTo(vg, w - pad - inset, inputTop);
		nvgStrokeColor(vg, nvgRGBA(0xff, 0xff, 0xff, 0x14));
		nvgStroke(vg);

		// Message log — monospace, newest at the bottom. Each chat line is word-wrapped into
		// one or more physical "rows"; we flatten them all, then show the window that fits.
		const float tpad = pad + inset, tclip = avail - 2 * inset;
		unsigned gen = module->chatGen.load(std::memory_order_acquire);
		if (gen != rowsGen || tclip != rowsClip) {
			rowsGen = gen;
			rowsClip = tclip;
			rows.clear();
			for (const Ninjam::ChatLine& l : module->chatSnapshot()) {
				if (l.kind == Ninjam::ChatLine::Topic) {
					for (const std::string& s : wrapText(vg, FONT_MONO, 14.f, tclip, l.text))
						rows.push_back({14.f, "", TH_CON_NAME, tpad, nvgRGB(0xe0, 0xc4, 0x6a), s});
				} else if (l.kind == Ninjam::ChatLine::System) {
					for (const std::string& s : wrapText(vg, FONT_MONO, 11.f, tclip, l.text))
						rows.push_back({11.f, "", TH_CON_NAME, tpad, TH_CON_DIM, s});
				} else if (l.who.empty()) {
					for (const std::string& s : wrapText(vg, FONT_MONO, 11.5f, tclip, l.text))
						rows.push_back({11.5f, "", TH_CON_NAME, tpad, TH_CON_TEXT, s});
				} else {
					// "name " coloured by ownership, then the wrapped message hung under the text.
					std::string name = l.who + " ";
					float indent = textWidth(vg, FONT_MONO, 11.5f, name);
					if (indent > tclip * 0.5f) indent = tclip * 0.5f; // keep room for the message
					NVGcolor nc = l.mine ? TH_CON_MINE : TH_CON_NAME;
					std::vector<std::string> ws = wrapText(vg, FONT_MONO, 11.5f, tclip - indent, l.text);
					for (size_t k = 0; k < ws.size(); k++)
						rows.push_back({11.5f, k == 0 ? name : "", nc, tpad + indent, TH_CON_TEXT, ws[k]});
				}
			}
		}
		if (rows.empty()) {
			drawTxt(vg, FONT_MONO, tpad, (logTop + logBot) / 2, 11.5f, TH_CON_DIM,
				"\xe2\x80\x94 no messages yet \xe2\x80\x94", NVG_ALIGN_LEFT);
			lastTotal = -1; // forget scroll state while empty so a fresh room starts pinned
			return;
		}

		// Clamp the scroll window. scrollUp counts rows lifted above the newest; 0 = pinned to
		// the bottom (auto-tail). When new rows arrive while scrolled up, grow scrollUp by the
		// same amount so the viewport holds on the messages being read.
		int total = (int) rows.size();
		int fit = (int) ((logBot - logTop) / lineH);
		if (fit < 1) fit = 1;
		int maxScroll = total > fit ? total - fit : 0;
		if (lastTotal >= 0 && scrollUp > 0 && total > lastTotal)
			scrollUp += total - lastTotal;
		lastTotal = total;
		if (scrollUp > maxScroll) scrollUp = maxScroll;
		if (scrollUp < 0) scrollUp = 0;
		int startRow = maxScroll - scrollUp;

		float y = logTop + lineH * 0.5f;
		for (int i = startRow; i < total && i < startRow + fit; i++) {
			const Row& r = rows[i];
			if (!r.name.empty())
				drawTxt(vg, FONT_MONO, tpad, y, r.size, r.nameCol, r.name, NVG_ALIGN_LEFT);
			drawTxt(vg, FONT_MONO, r.bodyX, y, r.size, r.bodyCol, r.body, NVG_ALIGN_LEFT,
				tclip - (r.bodyX - tpad));
			y += lineH;
		}
	}
};

// "▾" button beside the server field: opens a menu of previously-joined servers.
struct ServerDropdownButton : HoverButton {
	Ninjam* module = nullptr;
	ui::TextField* serverField = nullptr;
	ui::TextField* userField = nullptr;
	ui::TextField* passField = nullptr;
	void onPress(const ButtonEvent& e) override {
		if (!module) {
			OpaqueWidget::onButton(e);
			return;
		}
		ui::Menu* menu = createMenu();
		if (module->serverHistory.empty()) {
			menu->addChild(createMenuLabel("No previous servers"));
		} else {
			ui::TextField* sf = serverField;
			ui::TextField* uf = userField;
			ui::TextField* pf = passField;
			Ninjam* m = module;
			for (const std::string& hp : module->serverHistory)
				menu->addChild(createMenuItem(hp, "", [sf, uf, pf, m, hp]() {
					if (sf) sf->text = hp;
					// Recall the stored username/password for that server.
					auto it = m->serverCreds.find(hp);
					if (it != m->serverCreds.end()) {
						if (uf) uf->text = it->second.user;
						if (pf) pf->text = it->second.pass;
					}
				}));
		}
		e.consume(this);
	}
	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 3);
		nvgFillColor(args.vg, hovered ? nvgRGBA(0x00, 0x00, 0x00, 0x18) : TH_WELL);
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, TH_CARD_BD);
		nvgStroke(args.vg);
		drawTxt(args.vg, FONT_REG, box.size.x / 2, box.size.y / 2, 13.f,
			TH_TEXT, "\xe2\x96\xbe", NVG_ALIGN_CENTER); // ▾
	}
};

// "JOIN" button — connects to the typed server with the entered credentials.
struct JoinButton : HoverButton {
	Ninjam* module = nullptr;
	ui::TextField* userField = nullptr;
	ui::TextField* passField = nullptr;
	ui::TextField* serverField = nullptr;
	void onPress(const ButtonEvent& e) override {
		if (module && module->joined)
			module->stopAll();          // already connected -> disconnect
		else
			directJoin(module, userField, passField, serverField);
		e.consume(this);
	}
	void draw(const DrawArgs& args) override {
		bool connected = module && module->joined;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 3);
		// Green = JOIN (connect); red/orange = LEAVE (disconnect). Solid fills on light panel.
		nvgFillColor(args.vg, connected
			? (hovered ? nvgRGB(0xd8, 0x5a, 0x2e) : nvgRGB(0xc8, 0x52, 0x2a))
			: (hovered ? nvgRGB(0x33, 0xbe, 0x62) : NINJAM_GREEN));
		nvgFill(args.vg);
		drawTxt(args.vg, FONT_BOLD, box.size.x / 2, box.size.y / 2, 12.f,
			nvgRGB(0xff, 0xff, 0xff), connected ? "LEAVE" : "JOIN", NVG_ALIGN_CENTER);
	}
};

// "Private server:" card — a titled box with username, masked password, host:port (with a
// history dropdown), and a JOIN button. Creates and lays out its children.
struct JoinCard : Widget {
	JoinCard(Ninjam* module, float width) {
		box.size = Vec(width, 48);
		const float inner = 8.f, gap = 5.f;

		ui::TextField* userField = new ui::TextField;
		ui::PasswordField* passField = new ui::PasswordField;
		ServerField* serverField = new ServerField;

		// Row 1: server host:port + history dropdown (where the old label was).
		const float dropW = 22.f;
		float serverW = width - 2 * inner - dropW - gap;
		serverField->box.pos = Vec(inner, 5);
		serverField->box.size = Vec(serverW, 18);
		serverField->placeholder = "host:port";
		serverField->module = module;
		serverField->userField = userField;
		serverField->passField = passField;
		serverField->text = (module && !module->joinHost.empty())
			? (module->joinHost + ":" + std::to_string(module->joinPort)) : "";
		addChild(serverField);

		ServerDropdownButton* drop = new ServerDropdownButton;
		drop->module = module;
		drop->serverField = serverField;
		drop->userField = userField;
		drop->passField = passField;
		drop->box.pos = Vec(inner + serverW + gap, 5);
		drop->box.size = Vec(dropW, 18);
		addChild(drop);

		// Row 2: username | password | JOIN.
		const float joinW = 46.f;
		float credW = (width - 2 * inner - joinW - 2 * gap) / 2.f;
		userField->box.pos = Vec(inner, 27);
		userField->box.size = Vec(credW, 18);
		userField->placeholder = "username";
		userField->text = module ? module->joinUser : "";
		addChild(userField);

		passField->box.pos = Vec(inner + credW + gap, 27);
		passField->box.size = Vec(credW, 18);
		passField->placeholder = "password";
		passField->text = module ? module->joinPass : "";
		addChild(passField);

		JoinButton* join = new JoinButton;
		join->module = module;
		join->userField = userField;
		join->passField = passField;
		join->serverField = serverField;
		join->box.pos = Vec(inner + 2 * credW + 2 * gap, 27);
		join->box.size = Vec(joinW, 18);
		addChild(join);

		// TAB / Shift-TAB cycles username -> password -> server -> username.
		userField->nextField = passField;   userField->prevField = serverField;
		passField->nextField = serverField; passField->prevField = userField;
		serverField->nextField = userField; serverField->prevField = passField;
	}
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, 4);
		nvgFillColor(vg, TH_CARD);
		nvgFill(vg);
		nvgStrokeColor(vg, TH_CARD_BD);
		nvgStroke(vg);
		Widget::draw(args);
	}
};

// Bottom I/O section: TRANSMIT IN row + peak meter + the two output rows. The jacks are
// added on top by NinjamWidget at matching x fractions / row y's. Label rows sit ~12 px
// above each jack row (IN ~26, audio ~68, CV ~104 in local coords).
struct OutputSection : Widget {
	Ninjam* module = nullptr;
	// L/R pairs grouped close together; the two stereo groups separated by a gap.
	// One jack row holds three stereo pairs: IN (left), MAIN (center), PLY (right);
	// the TX LED tucks in the gap right after the IN pair. The three plates share the
	// CV row's outer edges (IN-L sits under CLICK, PLY-R under PHASE) and are evenly spaced.
	//
	// Outer jack centers are placed so the group plates clear the panel edges by Radio's
	// 3.9 mm plate margin: groupBox extends jh+pad = 18 px past the outer center, so the
	// outer center sits at (mm2px(3.9)+18)/300 ≈ 0.0984 of the 20 HP (300 px) width. The
	// rest are a linear remap of the old 0.12…0.88 layout onto 0.0984…0.9016.
	static constexpr float xInL = 0.0984f, xInR = 0.2464f, xTx = 0.1724f;     // IN pair; TX LED centered between them
	static constexpr float xMainL = 0.4577f, xMainR = 0.6057f, xPolyL = 0.7536f, xPolyR = 0.9016f; // out pairs (MAIN nudged toward PLY)
	static constexpr float xClick = 0.0984f, xClock = 0.2992f, xReset = 0.5000f, xRun = 0.7008f, xPhase = 0.9016f;
	// A rounded jack-group box. `plate` = solid black output plate (white labels);
	// otherwise a light recessed well with a colored border (inputs).
	static void groupBox(NVGcontext* vg, float w, float xL, float xR, float top, float h,
	                     bool plate, NVGcolor border) {
		const float jh = 13.f, pad = 5.f;
		float x = w * xL - jh - pad, rx = w * xR + jh + pad;
		nvgBeginPath(vg);
		// Corner radius = Radio's output plate (the core/Fundamental value) so both
		// modules read as one plugin family.
		nvgRoundedRect(vg, x, top, rx - x, h, mm2px(AK_PLATE_R_MM));
		nvgFillColor(vg, plate ? AK_PLATE : TH_WELL);
		nvgFill(vg);
		nvgStrokeColor(vg, border);
		nvgStrokeWidth(vg, 1.f);
		nvgStroke(vg);
	}
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		const float w = box.size.x;
		// Plates mirror Radio's output plate exactly (Theme.hpp AK_PLATE_* constants) so
		// the two modules read identically. The CV row matches Radio's plate/jack; the
		// audio row is the same plate raised by one row spacing, so the IN/MAIN/PLY jacks
		// line up with the neighbours' input row and the CV row with their output row.
		// Y() converts an absolute-panel mm to this section's local coords; H() is a mm span.
		auto Y = [&](float mm) { return mm2px(mm) - box.pos.y; };
		auto H = [&](float mm) { return mm2px(mm); };
		const float rowSpan = AK_ROW_OUT_MM - AK_ROW_CV_MM;                              // one jack-row spacing
		const float pTopHi = Y(AK_PLATE_TOP_MM - rowSpan), pTopLo = Y(AK_PLATE_TOP_MM);  // plate tops: audio / CV
		const float pH = H(AK_PLATE_H_MM);
		const float labHi = Y(AK_PLATE_TOP_MM - rowSpan + AK_PLATE_LABEL_DY_MM);
		const float labLo = Y(AK_PLATE_TOP_MM + AK_PLATE_LABEL_DY_MM);

		// INPUT group: light well, blue border, dark bold labels — same row as the outputs.
		groupBox(vg, w, xInL, xInR, pTopHi, pH, false, TH_IN_BD);
		drawTxt(vg, FONT_BOLD, w * xInL, labHi, 11.f, TH_TEXT_DIM, "L", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, w * xInR, labHi, 11.f, TH_TEXT_DIM, "R", NVG_ALIGN_CENTER);

		// Peak meter (thin bar above the jack rows, aligned to the plate edges), in the gap
		// between the panel content above and the plate tops.
		const float bx = w * xClick - 18.f, by = Y(85.0f), bw = (w * xPhase + 18.f) - bx, bh = 4;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, bx, by, bw, bh, 2);
		nvgFillColor(vg, nvgRGBA(0, 0, 0, 0x33));
		nvgFill(vg);
		float p = module ? module->peak.load(std::memory_order_relaxed) : 0.f;
		p = std::max(0.f, std::min(1.f, p));
		if (p > 0.001f) {
			NVGcolor c = p > 0.95f ? nvgRGB(0xe0, 0x4a, 0x3a)
			           : p > 0.80f ? nvgRGB(0xe0, 0xc0, 0x3a)
			                       : NINJAM_GREEN;
			nvgBeginPath(vg);
			nvgRoundedRect(vg, bx, by, bw * p, bh, 2);
			nvgFillColor(vg, c);
			nvgFill(vg);
		}

		// OUTPUTS: black plates with bold white labels.
		const NVGcolor bd = nvgRGBA(0, 0, 0, 0x55);
		groupBox(vg, w, xMainL, xMainR, pTopHi, pH, true, bd);
		groupBox(vg, w, xPolyL, xPolyR, pTopHi, pH, true, bd);
		drawTxt(vg, FONT_BOLD, w * xMainL, labHi, 11.f, AK_PLATE_TEXT, "MAIN L", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, w * xMainR, labHi, 11.f, AK_PLATE_TEXT, "MAIN R", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, w * xPolyL, labHi, 11.f, AK_PLATE_TEXT, "PLY L", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, w * xPolyR, labHi, 11.f, AK_PLATE_TEXT, "PLY R", NVG_ALIGN_CENTER);

		groupBox(vg, w, xClick, xPhase, pTopLo, pH, true, bd);
		drawTxt(vg, FONT_BOLD, w * xClick, labLo, 9.5f, AK_PLATE_TEXT, "CLICK", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, w * xClock, labLo, 9.5f, AK_PLATE_TEXT, "CLOCK", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, w * xReset, labLo, 9.5f, AK_PLATE_TEXT, "RESET", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, w * xRun, labLo, 9.5f, AK_PLATE_TEXT, "RUN", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, w * xPhase, labLo, 9.5f, AK_PLATE_TEXT, "PHASE", NVG_ALIGN_CENTER);

		// "AK" maker mark below the output jacks, in the spot VCV uses for its logo —
		// large + bold, at Radio's exact AK row.
		drawTxt(vg, FONT_BOLD, w / 2.f, Y(AK_MARK_Y_MM), 16.f, TH_TEXT, "AK", NVG_ALIGN_CENTER);
	}
};

// Patch-load "you're silent!" takeover. The auto-rejoin path deliberately never
// auto-enables TX (a shared patch must not broadcast the user's input), which means a
// reloaded patch can sit in a room with an instrument wired in while nobody hears a
// note. When that state holds (txNudge && joined && !transmitting && IN connected —
// see NinjamWidget::step), this covers the jam view with a big pulsing yellow
// START TRANSMITTING banner and an arrow whose tip rests on the TX LED. Clicking
// anywhere on it (or the LED itself) starts TX and retires the nudge for the session.
struct TxNudge : Widget {
	Ninjam* module = nullptr;
	float ledX = 0.f; // TX LED center, local coords (set by the widget ctor)
	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			if (module) {
				module->transmitting = true;
				module->txNudge = false;
			}
			e.consume(this);
		}
	}
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		const float w = box.size.x, h = box.size.y;
		// Slow pulse so it reads as "act on me", not decoration.
		const float pulse = 0.70f + 0.30f * std::sin((float) system::getTime() * 4.f);
		const NVGcolor yel = nvgRGB(0xff, 0xd2, 0x1e);
		const NVGcolor yelPulse = nvgRGBA(0xff, 0xd2, 0x1e, (unsigned char) (0xff * pulse));

		// Banner card over the chat console.
		const float bx = 10.f, bw = w - 2 * bx, bh = 86.f, by = (h - bh) * 0.42f;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, bx, by, bw, bh, 6.f);
		nvgFillColor(vg, nvgRGBA(0x14, 0x10, 0x00, 0xe8));
		nvgFill(vg);
		nvgStrokeWidth(vg, 2.f);
		nvgStrokeColor(vg, yelPulse);
		nvgStroke(vg);
		drawTxt(vg, FONT_BOLD, w / 2, by + 22.f, 23.f, yel, "START", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, w / 2, by + 47.f, 23.f, yel, "TRANSMITTING", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_REG, w / 2, by + bh - 15.f, 10.f, nvgRGBA(0xff, 0xd2, 0x1e, 0xb8),
			"rejoined by the patch \xe2\x80\x94 the room can't hear you", NVG_ALIGN_CENTER);

		// Arrow: banner bottom → TX LED. The curve straightens to vertical at the tip
		// (control points share the tip's x), so the head points straight down at the LED.
		const float sy = by + bh + 5.f, ey = h - 1.f;
		nvgBeginPath(vg);
		nvgMoveTo(vg, w / 2, sy);
		nvgBezierTo(vg, w / 2, sy + (ey - sy) * 0.55f, ledX, ey - (ey - sy) * 0.45f, ledX, ey - 11.f);
		nvgStrokeWidth(vg, 4.f);
		nvgLineCap(vg, NVG_ROUND);
		nvgStrokeColor(vg, yelPulse);
		nvgStroke(vg);
		nvgBeginPath(vg);
		nvgMoveTo(vg, ledX, ey);
		nvgLineTo(vg, ledX - 7.f, ey - 12.f);
		nvgLineTo(vg, ledX + 7.f, ey - 12.f);
		nvgClosePath(vg);
		nvgFillColor(vg, yelPulse);
		nvgFill(vg);
	}
};

struct NinjamWidget : ModuleWidget {
	Ninjam* nj = nullptr;
	// State-dependent widgets: connect UI (shown when disconnected) vs jam view (connected).
	Widget* joinCard = nullptr;
	Widget* search = nullptr;
	Widget* refresh = nullptr;
	Widget* browser = nullptr;
	Widget* jamView = nullptr;
	Widget* metro = nullptr;
	Widget* chatField = nullptr;
	Widget* txNudge = nullptr;

	NinjamWidget(Ninjam* module) {
		nj = module;
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Ninjam.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		const float W = box.size.x;

		// Always-on top status bar.
		TransportBlock* transport = new TransportBlock;
		transport->module = module;
		transport->box.pos = Vec(6, 16);
		transport->box.size = Vec(W - 12, 26);
		addChild(transport);

		// Clickable status LED (top-right): green=live, amber=connecting, red=stopped.
		ClickableLed* led = new ClickableLed;
		led->box.size = Vec(13, 13);
		led->box.pos = Vec(W - 6 - 4 - 13, 22);
		led->isLive = [module]() { return module && module->ledLive(); };
		led->isPending = [module]() { return module && module->ledPending(); };
		led->onClick = [module]() { if (module && module->isActive()) module->stopAll(); };
		addChild(led);

		// Metronome toggle (left of the LED) — shown only when joined.
		MetronomeToggle* m = new MetronomeToggle;
		m->module = module;
		m->box.size = Vec(16, 16);
		m->box.pos = Vec(W - 6 - 4 - 13 - 6 - 16, 21);
		addChild(m);
		metro = m;

		// ---- Disconnected: server-selection UI ----
		JoinCard* card = new JoinCard(module, W - 12);
		card->box.pos = Vec(6, 46);
		addChild(card);
		joinCard = card;

		SearchField* s = new SearchField;
		s->module = module;
		s->box.pos = Vec(8, 100);
		s->box.size = Vec(W - 8 - 40, 20);
		addChild(s);
		search = s;

		RefreshButton* rb = new RefreshButton;
		rb->module = module;
		rb->box.pos = Vec(W - 36, 100);
		rb->box.size = Vec(28, 20);
		addChild(rb);
		refresh = rb;

		// Room list fills all the way down to just above the jack section.
		RoomBrowser* b = new RoomBrowser;
		b->module = module;
		b->search = s;
		b->box.pos = Vec(6, 124);
		b->box.size = Vec(W - 12, 242 - 124);
		addChild(b);
		browser = b;

		// ---- Connected: jam view (same region; replaces the connect UI) ----
		// The jam view starts just under the status bar (tight) and runs down to just above
		// the meter/jack section, so the beat ticks + chat console fill the space.
		const float jvTop = 44.f, jvH = 198.f;
		JamView* jv = new JamView;
		jv->module = module;
		jv->box.pos = Vec(6, jvTop);
		jv->box.size = Vec(W - 12, jvH);
		addChild(jv);
		jamView = jv;

		// Chat input — frameless, overlaying the bottom strip of the console pane. Taller
		// than a default field so descenders aren't clipped.
		ChatField* cf = new ChatField;
		cf->module = module;
		cf->box.pos = Vec(20, jvTop + jvH - 23);
		cf->box.size = Vec(W - 40, 19);
		addChild(cf);
		chatField = cf;

		// Bottom I/O section (always): TRANSMIT IN row + meter + output rows.
		const float oy = 200;
		OutputSection* out = new OutputSection;
		out->module = module;
		out->box.pos = Vec(0, oy);
		out->box.size = Vec(W, 180); // down to the panel bottom, room for the "AK" mark
		addChild(out);

		// Jack rows at Radio's exact grid: audio/IN row = Radio's CV-input row, CV row =
		// Radio's output row (Theme.hpp) — absolute panel mm, independent of oy.
		const float rowA = mm2px(AK_ROW_CV_MM), rowB = mm2px(AK_ROW_OUT_MM);
		// One row: TRANSMIT IN (poly) + TX LED, MAIN out, PLY out.
		addInput(createInputCentered<PJ301MPort>(Vec(W * OutputSection::xInL, rowA), module, Ninjam::LEFT_INPUT));
		addInput(createInputCentered<PJ301MPort>(Vec(W * OutputSection::xInR, rowA), module, Ninjam::RIGHT_INPUT));
		TxToggle* txBtn = new TxToggle;
		txBtn->module = module;
		txBtn->box.size = Vec(11, 11);
		// Center it in the IN well: horizontally between the two jacks, vertically between
		// the L/R label row and the jack row (rowA), nudged up a few px off the jacks.
		const float labelRow = mm2px(AK_PLATE_TOP_MM - (AK_ROW_OUT_MM - AK_ROW_CV_MM) + AK_PLATE_LABEL_DY_MM);
		txBtn->box.pos = Vec(W * OutputSection::xTx - 5.5f, (labelRow + rowA) / 2.f - 5.5f - 4.f);
		addChild(txBtn);
		// Outputs: MAIN L/R + PLAYERS poly L/R, then CLICK/CLOCK/RESET/RUN/PHASE.
		addOutput(createOutputCentered<PJ301MPort>(Vec(W * OutputSection::xMainL, rowA), module, Ninjam::LEFT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(Vec(W * OutputSection::xMainR, rowA), module, Ninjam::RIGHT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(Vec(W * OutputSection::xPolyL, rowA), module, Ninjam::POLY_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(Vec(W * OutputSection::xPolyR, rowA), module, Ninjam::POLY_R_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(Vec(W * OutputSection::xClick, rowB), module, Ninjam::CLICK_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(Vec(W * OutputSection::xClock, rowB), module, Ninjam::CLOCK_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(Vec(W * OutputSection::xReset, rowB), module, Ninjam::RESET_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(Vec(W * OutputSection::xRun, rowB), module, Ninjam::RUN_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(Vec(W * OutputSection::xPhase, rowB), module, Ninjam::PHASE_OUTPUT));

		// Patch-load TX nudge — added last so it draws over everything it spans (jam
		// view, chat field, peak meter). Runs from under the status bar down to the IN
		// well's plate top, so the arrow tip lands just above the TX LED.
		TxNudge* tn = new TxNudge;
		tn->module = module;
		const float plateTopHi = mm2px(AK_PLATE_TOP_MM - (AK_ROW_OUT_MM - AK_ROW_CV_MM));
		tn->box.pos = Vec(6, jvTop);
		tn->box.size = Vec(W - 12, plateTopHi - 3.f - jvTop);
		tn->ledX = W * OutputSection::xTx - tn->box.pos.x;
		tn->visible = false;
		addChild(tn);
		txNudge = tn;
	}

	// Swap connect UI <-> jam view each frame; keep our broadcast channels in sync.
	void step() override {
		// If we still believe we're joined but the JOIN bg thread has exited on its own, the
		// server dropped us (kick / shutdown / network loss) or auth was rejected. Reconcile on
		// the UI thread so the panel returns to the connect view and reports the reason.
		if (nj && nj->joined && !nj->njclient.isRunning())
			nj->handleServerDrop();
		// Same idea for LISTEN: a terminal Icecast error (unpublished mount / 404 / server drop)
		// while we still think we're listening → reconcile so the LED leaves amber, the reason
		// shows on the status bar, and the room browser returns for another pick.
		if (nj && nj->listenFailed())
			nj->handleListenError();
		bool joined = nj && nj->joined;
		if (joinCard) joinCard->visible = !joined;
		if (search) search->visible = !joined;
		if (refresh) refresh->visible = !joined;
		if (browser) browser->visible = !joined;
		if (jamView) jamView->visible = joined;
		if (metro) metro->visible = joined;
		if (chatField) chatField->visible = joined;
		// The TX nudge shows only while the patch-load rejoin's "silent in a room with
		// an instrument plugged in" state actually holds — plugging IN in later (cables
		// restore after dataFromJson) reveals it, any TX decision retires it.
		if (txNudge)
			txNudge->visible = joined
				&& nj->txNudge.load(std::memory_order_relaxed)
				&& !nj->transmitting.load(std::memory_order_relaxed)
				&& nj->inputs[Ninjam::LEFT_INPUT].isConnected();
		if (nj) nj->syncTransmit();
		ModuleWidget::step();
	}

	void appendContextMenu(Menu* menu) override {
		Ninjam* module = getModule<Ninjam>();
		if (!module)
			return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Refresh room list", "", [module]() {
			module->directory.refresh();
		}));
		menu->addChild(createMenuItem("Stop", "", [module]() {
			if (module->isActive()) module->stopAll();
		}));
		menu->addChild(createCheckMenuItem("Metronome click", "",
			[module]() { return module->clickEnabled.load(std::memory_order_relaxed); },
			[module]() { module->clickEnabled = !module->clickEnabled; }));

		// CLOCK output resolution. 1 = once per beat (step sequencers); 2 (default) for
		// modules that detect tempo assuming 2 pulses/beat; 24 ≈ MIDI-clock sync.
		menu->addChild(createSubmenuItem("Clock: pulses per beat", "", [module](Menu* sub) {
			static const int opts[] = {1, 2, 4, 8, 24};
			for (int ppqn : opts) {
				sub->addChild(createCheckMenuItem(string::f("%d / beat", ppqn), "",
					[module, ppqn]() { return module->clockPpqn.load(std::memory_order_relaxed) == ppqn; },
					[module, ppqn]() { module->clockPpqn.store(ppqn, std::memory_order_relaxed); }));
			}
		}));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Transmit quality"));
		struct QOpt { const char* label; float q; };
		static const QOpt qopts[] = {
			{"Low (~135 kbps)", 0.3f}, {"Medium (~190 kbps)", 0.5f}, {"High (~235 kbps)", 0.7f}};
		for (const QOpt& o : qopts) {
			float q = o.q;
			menu->addChild(createCheckMenuItem(o.label, "",
				[module, q]() { return std::fabs(module->txQuality - q) < 0.01f; },
				[module, q]() { module->txQuality = q; module->txDeclared = -1; })); // re-declare w/ new q
		}
		// NINJAM voice-chat mode (canonical channel flag 2): capture starts instantly and
		// receivers play it live (~sub-second + network) instead of interval-aligned. For
		// talkback / latency testing — it deliberately opts this channel out of jam sync.
		menu->addChild(createCheckMenuItem("Voice mode (live, unsynced)", "",
			[module]() { return module->txVoice.load(std::memory_order_relaxed); },
			[module]() {
				module->txVoice = !module->txVoice.load(std::memory_order_relaxed);
			})); // syncTransmit() re-declares on the next widget step
	}
};

Model* modelNinjam = createModel<Ninjam, NinjamWidget>("Ninjam");
