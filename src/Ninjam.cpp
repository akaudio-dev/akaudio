#include "plugin.hpp"
#include "net/Stream.hpp"
#include "net/RoomDirectory.hpp"
#include "net/ninjam/NjClient.hpp"
#include "ClickableLed.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <mutex>
#include <vector>

// NINJAM module — two ways to hear a jam:
//
//  • LISTEN (Icecast stream): the zero-dependency path. Public NINJAM communities
//    (ninjamer.com, ninbot, …) publish a live Icecast/HTTP mix of each room, so we
//    consume it like internet radio (net/Stream.hpp, same as Radio). No protocol, no
//    join — pure external listening.
//  • JOIN (NINJAM protocol): connect to the server over the real NINJAM protocol
//    (net/ninjam/NjClient), authenticate, subscribe, and decode the live multi-user
//    OGG interval mix ourselves — the actual jam, no Icecast required. Listen-only for
//    now (we transmit nothing); speaking in the jam is a later phase.
//
// Both feed the same lock-free ring → process() just pull()s from whichever is active.
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

	int mode = MODE_LISTEN;

	// ---- LISTEN (Icecast) state ----
	// The jam's Icecast/HTTP listen-stream URL (MP3). Set by picking a room or typing
	// one; empty by default since rooms come and go.
	std::string url = "";
	bool listening = false;

	// ---- JOIN (protocol) state ----
	std::string joinHost = "";
	int joinPort = 2049;
	std::string joinUser = "";  // display name; empty -> anonymous
	std::string joinPass = "";  // empty -> anonymous; set for a registered/private server
	bool joined = false;
	// Most-recent-first "host:port" of servers we've joined, for the panel dropdown.
	std::vector<std::string> serverHistory;

	// ---- Beat clock + metronome (meaningful only when joined to a tempo) ----
	bool clickEnabled = false;            // metronome audible-click toggle (persisted)
	std::atomic<int> currentBeat{-1};     // UI reads this; -1 = idle
	std::atomic<float> jamPhase{0.f};     // 0..1 interval progress (UI progress bar)
	std::atomic<bool> resyncBeat{false};  // set on join / tempo change to reset the clock
	// process()-thread only beat/click state:
	double beatPhase = 0.0;
	int beatIndex = 0;
	float clickEnv = 0.f, clickPhase = 0.f, clickFreq = 880.f, clickAmp = 0.f;

	// Human label for the picked room (room name, or host). Shown on the panel and
	// persisted; shared across modes since only one is active at a time.
	std::string roomLabel = "";

	// Live jam metadata from the protocol client (written by its net thread).
	std::atomic<int> jamBpm{0};
	std::atomic<int> jamBpi{0};
	std::mutex rosterMutex;
	std::vector<std::string> roster; // "name" of active remote users (UI thread reads)

	// Decaying output peak [0,1] for the on-panel level meter. Written by the audio
	// thread, read by the UI thread; relaxed atomics are fine for a meter.
	std::atomic<float> peak{0.f};

	// ---- Transmit ----
	bool transmitting = false;  // TX enable (persisted)
	float txQuality = 0.5f;     // encoder VBR quality (persisted; ~190 kbps)
	int txDeclared = -1;        // last channel count declared to the server (UI thread)
	std::atomic<bool> txArmed{false}; // capture armed at a downbeat (aligns intervals)

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
		directory.refresh();
	}

	// Global (cross-instance) recall of the last server/credentials/history, so a brand-new
	// module is prefilled. Stored in the Rack user dir. dataFromJson (a saved patch) still
	// overrides these per-instance. Note: the password is stored here in plaintext (local).
	static std::string globalConfigPath() { return asset::user("akaudio-ninjam.json"); }

	void loadGlobal() {
		json_error_t err;
		json_t* root = json_load_file(globalConfigPath().c_str(), 0, &err);
		if (!root)
			return;
		if (json_t* j = json_object_get(root, "joinUser")) joinUser = json_string_value(j);
		if (json_t* j = json_object_get(root, "joinPass")) joinPass = json_string_value(j);
		if (json_t* j = json_object_get(root, "joinHost")) joinHost = json_string_value(j);
		if (json_t* j = json_object_get(root, "joinPort")) joinPort = (int) json_integer_value(j);
		if (json_t* hist = json_object_get(root, "serverHistory")) {
			serverHistory.clear();
			size_t i; json_t* v;
			json_array_foreach(hist, i, v)
				if (const char* s = json_string_value(v)) serverHistory.push_back(s);
		}
		json_decref(root);
	}

	void saveGlobal() {
		json_t* root = json_object();
		json_object_set_new(root, "joinUser", json_string(joinUser.c_str()));
		json_object_set_new(root, "joinPass", json_string(joinPass.c_str()));
		json_object_set_new(root, "joinHost", json_string(joinHost.c_str()));
		json_object_set_new(root, "joinPort", json_integer(joinPort));
		json_t* hist = json_array();
		for (const std::string& s : serverHistory)
			json_array_append_new(hist, json_string(s.c_str()));
		json_object_set_new(root, "serverHistory", hist);
		json_dump_file(root, globalConfigPath().c_str(), JSON_INDENT(2));
		json_decref(root);
	}

	// NjClient callbacks (fire on its background thread). Keep them light.
	akaudio::nj::NjClient::Callbacks jamCallbacks() {
		akaudio::nj::NjClient::Callbacks cb;
		// Log only the abnormal (errors / unexpected disconnects). onLog only fires for
		// anomalies; onState logs only the Error state. Normal connect/auth/subscribe/stop
		// stay silent. (Verbose per-event INFO logging available behind the commented line.)
		cb.onState = [](akaudio::nj::NjClient::State s, const std::string& msg) {
			if (s == akaudio::nj::NjClient::State::Error)
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
		return cb;
	}

	// ---- Mode-agnostic helpers (dispatch on `mode`) ----

	// Stop whatever is currently playing.
	void stopAll() {
		if (listening) { stream.stop(); listening = false; }
		if (joined) {
			njclient.stop();
			joined = false;
			jamBpm.store(0, std::memory_order_relaxed);
			jamBpi.store(0, std::memory_order_relaxed);
			std::lock_guard<std::mutex> lock(rosterMutex);
			roster.clear();
		}
	}

	void setMode(int m) {
		if (m == mode)
			return;
		stopAll();
		mode = m;
	}

	// ---- Per-room actions (the loudspeaker / enter icons on each row) ----
	void startListen(const akaudio::Room& room) {
		stopAll();
		mode = MODE_LISTEN;
		url = room.playUrl();
		roomLabel = room.name.empty() ? room.host : room.name;
		listen();
	}
	void startJoin(const akaudio::Room& room) {
		stopAll();
		mode = MODE_JOIN;
		joinHost = room.host;
		joinPort = room.port;
		roomLabel = room.name.empty() ? room.host : room.name;
		joinStart();
	}
	// Direct join to a typed server (private/registered). joinUser/joinPass must be set
	// by the caller (the panel fields) beforehand.
	void joinManual(const std::string& host, int port) {
		if (host.empty())
			return;
		stopAll();
		mode = MODE_JOIN;
		joinHost = host;
		joinPort = port > 0 ? port : 2049;
		roomLabel = joinHost;
		joinStart();
	}

	// ---- LISTEN (Icecast) ----
	void listen() {
		if (url.empty())
			return;
		stream.start(url);
		listening = true;
	}

	// ---- JOIN (protocol) ----
	void joinStart() {
		if (joinHost.empty())
			return;
		{
			std::lock_guard<std::mutex> lock(rosterMutex);
			roster.clear();
		}
		jamBpm.store(0, std::memory_order_relaxed);
		jamBpi.store(0, std::memory_order_relaxed);
		njclient.setSampleRate(APP->engine->getSampleRate());
		resyncBeat.store(true, std::memory_order_release);
		njclient.start(joinHost, joinPort, joinUser, joinPass, jamCallbacks());
		joined = true;
		addServerHistory(joinHost + ":" + std::to_string(joinPort));
		saveGlobal(); // remember this server/creds for the next fresh module
	}

	// Declare/update our broadcast channels to match the TX toggle + connected poly input.
	// Called from the widget step() (UI thread); does no per-sample / audio-thread work.
	void syncTransmit() {
		int desired = 0;
		if (transmitting && joined && inputs[LEFT_INPUT].isConnected()) {
			int nin = inputs[LEFT_INPUT].getChannels();
			if (nin < 1) nin = 1;
			desired = std::min(nin, akaudio::nj::NjAudio::MAX_TX);
		}
		if (desired != txDeclared) {
			std::vector<std::string> names;
			for (int i = 0; i < desired; i++) names.push_back("ch" + std::to_string(i + 1));
			njclient.setTransmit(names, txQuality);
			txDeclared = desired;
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
		if (serverHistory.size() > 12)
			serverHistory.resize(12);
	}

	bool isActive() const { return listening || joined; }

	bool isListeningTo(const akaudio::Room& room) const {
		return listening && url == room.playUrl();
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
		if (bpm > 0)
			return string::f("%s \xc2\xb7 %d BPM \xc2\xb7 %d BPI \xc2\xb7 %d here", sn, bpm, bpi, (int) n);
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
		float l = 0.f, r = 0.f; // main mix (= sum of players in JOIN, or the Icecast stream)
		if (mode == MODE_JOIN) {
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
			stream.pull(l, r); // leaves l/r at 0 on underrun
		}
		outputs[LEFT_OUTPUT].setVoltage(l * 5.f);
		outputs[RIGHT_OUTPUT].setVoltage(r * 5.f);

		// Peak meter: fast attack, ~150 ms exponential release.
		float amp = std::max(std::fabs(l), std::fabs(r));
		float p = peak.load(std::memory_order_relaxed);
		p = amp > p ? amp : p * std::exp(-args.sampleTime / 0.15f);
		peak.store(p, std::memory_order_relaxed);

		// ---- Beat clock + metronome (only when joined to a tempo) ----
		int bpmL = jamBpm.load(std::memory_order_relaxed);
		int bpiL = jamBpi.load(std::memory_order_relaxed);
		float click = 0.f, clockG = 0.f, resetG = 0.f, runG = 0.f, phaseV = 0.f;
		if (bpmL > 0 && bpiL > 0) {
			if (resyncBeat.exchange(false, std::memory_order_acq_rel)) {
				beatPhase = 0.0;
				beatIndex = 0;
				currentBeat.store(0, std::memory_order_relaxed);
				clickEnv = 0.f;
			}
			double spb = 60.0 * args.sampleRate / (double) bpmL; // samples per beat
			beatPhase += 1.0;
			if (beatPhase >= spb) {
				beatPhase -= spb;
				beatIndex = (beatIndex + 1) % bpiL;
				currentBeat.store(beatIndex, std::memory_order_relaxed);
				// Arm transmit capture on the downbeat so uploaded intervals align to the grid.
				if (beatIndex == 0 && transmitting && joined && txDeclared > 0)
					txArmed.store(true, std::memory_order_relaxed);
				clickEnv = 1.f; // arm a click; accent the downbeat
				clickPhase = 0.f;
				clickFreq = (beatIndex == 0) ? 1760.f : 880.f;
				clickAmp = (beatIndex == 0) ? 0.9f : 0.55f;
			}
			if (currentBeat.load(std::memory_order_relaxed) < 0)
				currentBeat.store(beatIndex, std::memory_order_relaxed);
			if (clickEnv > 0.f) {
				if (clickEnabled) {
					click = std::sin(2.f * (float) M_PI * clickPhase) * clickEnv * clickAmp;
					clickPhase += clickFreq * args.sampleTime;
					if (clickPhase >= 1.f) clickPhase -= 1.f;
				}
				clickEnv -= args.sampleTime / 0.035f; // ~35 ms decay
				if (clickEnv < 0.f) clickEnv = 0.f;
			}
			// CV sync outs: CLOCK = 50%-duty gate per beat; RESET = same pulse but only on
			// the downbeat; RUN high while playing; PHASE = 0-10V ramp across the interval.
			float beatFrac = (float) (beatPhase / spb);                 // 0..1 within beat
			float intervalPhase = ((float) beatIndex + beatFrac) / (float) bpiL; // 0..1
			clockG = (beatFrac < 0.5f) ? 10.f : 0.f;
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
		// ---- Transmit capture: feed input frames once armed at a downbeat ----
		if (transmitting && joined && txDeclared > 0 && txArmed.load(std::memory_order_relaxed)
		        && inputs[LEFT_INPUT].isConnected()) {
			bool rConn = inputs[RIGHT_INPUT].isConnected();
			for (int ch = 0; ch < txDeclared; ch++) {
				float il = inputs[LEFT_INPUT].getPolyVoltage(ch) * 0.2f;  // ±5V -> ±1
				float ir = rConn ? inputs[RIGHT_INPUT].getPolyVoltage(ch) * 0.2f : il;
				njclient.captureFrame(ch, il, ir);
			}
		}

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
		json_object_set_new(root, "roomLabel", json_string(roomLabel.c_str()));
		json_object_set_new(root, "listening", json_boolean(listening));
		json_object_set_new(root, "joinHost", json_string(joinHost.c_str()));
		json_object_set_new(root, "joinPort", json_integer(joinPort));
		json_object_set_new(root, "joinUser", json_string(joinUser.c_str()));
		json_object_set_new(root, "joinPass", json_string(joinPass.c_str()));
		json_object_set_new(root, "joined", json_boolean(joined));
		json_object_set_new(root, "clickEnabled", json_boolean(clickEnabled));
		json_object_set_new(root, "transmitting", json_boolean(transmitting));
		json_object_set_new(root, "txQuality", json_real(txQuality));
		json_t* hist = json_array();
		for (const std::string& s : serverHistory)
			json_array_append_new(hist, json_string(s.c_str()));
		json_object_set_new(root, "serverHistory", hist);
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "mode"))
			mode = (int) json_integer_value(j);
		if (json_t* j = json_object_get(root, "url"))
			url = json_string_value(j);
		if (json_t* j = json_object_get(root, "roomLabel"))
			roomLabel = json_string_value(j);
		if (json_t* j = json_object_get(root, "listening"))
			listening = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "joinHost"))
			joinHost = json_string_value(j);
		if (json_t* j = json_object_get(root, "joinPort"))
			joinPort = (int) json_integer_value(j);
		if (json_t* j = json_object_get(root, "joinUser"))
			joinUser = json_string_value(j);
		if (json_t* j = json_object_get(root, "joinPass"))
			joinPass = json_string_value(j);
		if (json_t* j = json_object_get(root, "joined"))
			joined = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "clickEnabled"))
			clickEnabled = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "transmitting"))
			transmitting = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "txQuality"))
			txQuality = (float) json_real_value(j);
		serverHistory.clear();
		if (json_t* hist = json_object_get(root, "serverHistory")) {
			size_t i;
			json_t* v;
			json_array_foreach(hist, i, v) {
				if (const char* s = json_string_value(v))
					serverHistory.push_back(s);
			}
		}

		// Auto-resume on patch load.
		if (mode == MODE_JOIN && joined) {
			joined = false; // joinStart() sets it
			joinStart();
		} else if (mode == MODE_LISTEN && listening) {
			listening = false; // listen() sets it
			listen();
		}
	}
};

// Parse "host[:port]" into host + port (default 2049).
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

// Shared connect action for the in-panel Direct Join card: pulls username/password from
// their fields and the host:port from the server field, then joins (private/registered
// servers that aren't in the public directory).
static void directJoin(Ninjam* module, ui::TextField* userF, ui::TextField* passF, ui::TextField* serverF) {
	if (!module || !serverF || serverF->text.empty())
		return;
	if (userF) module->joinUser = userF->text;
	if (passF) module->joinPass = passF->text;
	std::string host;
	int port;
	parseHostPort(serverF->text, host, port);
	module->joinManual(host, port);
}

// Text field that moves focus to the next/previous field on TAB / Shift-TAB.
struct TabField : ui::TextField {
	Widget* nextField = nullptr;
	Widget* prevField = nullptr;
	void onSelectKey(const SelectKeyEvent& e) override {
		if ((e.action == GLFW_PRESS || e.action == GLFW_REPEAT) && e.key == GLFW_KEY_TAB) {
			Widget* t = (e.mods & GLFW_MOD_SHIFT) ? prevField : nextField;
			if (t) {
				APP->event->setSelectedWidget(t);
				e.consume(this);
				return;
			}
		}
		ui::TextField::onSelectKey(e);
	}
};

// Server "host:port" field — pressing Enter connects, same as the Join button.
struct ServerField : TabField {
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

static const char* FONT_BOLD = "res/fonts/Nunito-Bold.ttf";
static const char* FONT_REG = "res/fonts/DejaVuSans.ttf";

static const NVGcolor NINJAM_GREEN = nvgRGB(0x2a, 0xa8, 0x55); // accent (darker for light panel)

// ---- Light "Fundamental-style" theme ----
static const NVGcolor TH_TEXT     = nvgRGB(0x24, 0x27, 0x2b); // primary dark text
static const NVGcolor TH_TEXT_DIM = nvgRGB(0x5c, 0x61, 0x68); // secondary text
static const NVGcolor TH_CARD     = nvgRGB(0xd6, 0xd9, 0xdc); // card/well fill (vs panel)
static const NVGcolor TH_CARD_BD  = nvgRGBA(0x00, 0x00, 0x00, 0x26); // card/well border
static const NVGcolor TH_WELL     = nvgRGBA(0x00, 0x00, 0x00, 0x10); // recessed area fill
static const NVGcolor TH_OUT_PLATE= nvgRGB(0x26, 0x29, 0x2e); // black output plate
static const NVGcolor TH_OUT_TEXT = nvgRGB(0xe9, 0xec, 0xef); // label on a black plate
static const NVGcolor TH_IN_BD    = nvgRGB(0x3c, 0x6f, 0xc4); // input frame stroke (blue)

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

// Draw left-aligned (or centered) text, vertically centered on y, optionally
// ellipsized to clipW. Loads the font by asset path each call (Rack caches it).
static void drawTxt(NVGcontext* vg, const char* fontRes, float x, float y, float size,
		NVGcolor col, const std::string& s, int halign = NVG_ALIGN_LEFT, float clipW = -1.f) {
	std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system(fontRes));
	if (!font || font->handle < 0)
		return;
	nvgFontFaceId(vg, font->handle);
	nvgFontSize(vg, size);
	nvgFillColor(vg, col);
	nvgTextAlign(vg, halign | NVG_ALIGN_MIDDLE);

	std::string t = s;
	if (clipW > 0.f) {
		float b[4];
		nvgTextBounds(vg, 0, 0, t.c_str(), NULL, b);
		if (b[2] - b[0] > clipW) {
			while (t.size() > 1) {
				t.pop_back();
				std::string u = t + "\xe2\x80\xa6"; // …
				nvgTextBounds(vg, 0, 0, u.c_str(), NULL, b);
				if (b[2] - b[0] <= clipW) {
					t = u;
					break;
				}
			}
		}
	}
	nvgText(vg, x, y, t.c_str(), NULL);
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
struct RoomRow : OpaqueWidget {
	Ninjam* module = nullptr;
	akaudio::Room room;
	bool hovered = false;
	int hoveredIcon = 0; // 0 none, 1 listen, 2 join

	static constexpr float ICON = 15.f;
	float listenCx() const { return box.size.x - 36.f; }
	float joinCx() const { return box.size.x - 15.f; }
	static constexpr float ICON_CY = 13.f;

	// 1 = listen icon, 2 = join icon, 0 = neither, for a point in local coords.
	int iconAt(math::Vec p) const {
		if (std::fabs(p.y - ICON_CY) > 11.f) return 0;
		if (std::fabs(p.x - listenCx()) <= 11.f) return 1;
		if (std::fabs(p.x - joinCx()) <= 11.f) return 2;
		return 0;
	}

	void onEnter(const EnterEvent& e) override { hovered = true; OpaqueWidget::onEnter(e); }
	void onLeave(const LeaveEvent& e) override { hovered = false; hoveredIcon = 0; OpaqueWidget::onLeave(e); }
	void onHover(const HoverEvent& e) override {
		hoveredIcon = iconAt(e.pos);
		OpaqueWidget::onHover(e);
	}
	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && module) {
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
		OpaqueWidget::onButton(e);
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
		std::string cap = room.userMax > 0 ? string::f("%d/%d", room.userCount, room.userMax)
		                                    : string::f("%d", room.userCount);
		std::string stats = string::f("%d BPM \xc2\xb7 %d BPI \xc2\xb7 ", room.bpm, room.bpi) + cap + " here";
		drawTxt(vg, FONT_REG, pad, 26, 11.5f, TH_TEXT_DIM, stats, NVG_ALIGN_LEFT, w - 2 * pad);

		// Players line (if anyone is in the room).
		if (!room.users.empty()) {
			std::string who;
			for (size_t i = 0; i < room.users.size(); i++) {
				if (i)
					who += ", ";
				who += room.users[i];
			}
			drawTxt(vg, FONT_REG, pad, 39, 10.f, nvgRGB(0x3a, 0x86, 0x55), who, NVG_ALIGN_LEFT, w - 2 * pad);
		}
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
			row->room = room;
			row->box.pos = Vec(0, y);
			row->box.size = Vec(w, room.users.empty() ? 31.f : 45.f);
			container->addChild(row);
			y += row->box.size.y;
		}
		container->box.size = Vec(w, y);
	}

	void step() override {
		if (module) {
			if (++refreshTimer >= 60 * 30) { // ~30 s at 60 fps
				refreshTimer = 0;
				module->directory.refresh();
			}
			unsigned g = module->directory.generation();
			std::string f = search ? lower(search->text) : "";
			if (g != lastGen || f != lastFilter) {
				lastGen = g;
				lastFilter = f;
				rebuild();
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
			std::string msg = (module && module->directory.loading())
				? "Loading rooms\xe2\x80\xa6"
				: "No rooms \xe2\x80\x94 try Refresh";
			drawTxt(args.vg, FONT_REG, box.size.x / 2, box.size.y / 2, 13.f,
				TH_TEXT_DIM, msg, NVG_ALIGN_CENTER);
		}
	}
};

// Compact search box.
struct SearchField : ui::TextField {
	SearchField() {
		placeholder = "Filter rooms or players\xe2\x80\xa6";
	}
};

// Clickable refresh control (re-fetches the directory in the background).
struct RefreshButton : OpaqueWidget {
	Ninjam* module = nullptr;
	bool hovered = false;

	void onEnter(const EnterEvent& e) override { hovered = true; OpaqueWidget::onEnter(e); }
	void onLeave(const LeaveEvent& e) override { hovered = false; OpaqueWidget::onLeave(e); }
	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			if (module)
				module->directory.refresh();
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
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
struct TxToggle : OpaqueWidget {
	Ninjam* module = nullptr;
	bool hovered = false;
	void onEnter(const EnterEvent& e) override { hovered = true; OpaqueWidget::onEnter(e); }
	void onLeave(const LeaveEvent& e) override { hovered = false; OpaqueWidget::onLeave(e); }
	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			if (module) module->transmitting = !module->transmitting;
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
	}
	void draw(const DrawArgs& args) override {
		bool on = module && module->transmitting;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 3);
		nvgFillColor(args.vg, on ? (hovered ? nvgRGB(0x33, 0xbe, 0x62) : NINJAM_GREEN)
		                         : (hovered ? nvgRGBA(0x00, 0x00, 0x00, 0x18) : TH_WELL));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, TH_CARD_BD);
		nvgStroke(args.vg);
		drawTxt(args.vg, FONT_BOLD, box.size.x / 2, box.size.y / 2, 11.f,
			on ? nvgRGB(0xff, 0xff, 0xff) : TH_TEXT_DIM, "TX", NVG_ALIGN_CENTER);
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
struct MetronomeToggle : OpaqueWidget {
	Ninjam* module = nullptr;
	bool hovered = false;
	void onEnter(const EnterEvent& e) override { hovered = true; OpaqueWidget::onEnter(e); }
	void onLeave(const LeaveEvent& e) override { hovered = false; OpaqueWidget::onLeave(e); }
	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			if (module) module->clickEnabled = !module->clickEnabled;
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
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
		drawTxt(vg, FONT_BOLD, 8, h / 2, 11.5f,
			joined ? nvgRGB(0x1c, 0x7a, 0x3e) : TH_TEXT_DIM,
			joined ? jam : module->directory.status(), NVG_ALIGN_LEFT, w - 8 - 44);
	}
};

// The connected "jam view" (replaces the server-selection UI once joined): big beat-tick
// row, an interval progress bar, and the roster of who's in the room.
struct JamView : Widget {
	Ninjam* module = nullptr;
	void draw(const DrawArgs& args) override {
		if (!module)
			return;
		NVGcontext* vg = args.vg;
		const float w = box.size.x, pad = 12.f, avail = w - 2 * pad;
		int bpi = module->jamBpi.load(std::memory_order_relaxed);
		int cur = module->currentBeat.load(std::memory_order_relaxed);
		float phase = module->jamPhase.load(std::memory_order_relaxed);

		// Big beat ticks.
		if (bpi > 0) {
			const float ty = 16.f, th = 30.f, gap = 3.f;
			float tw = (avail - (bpi - 1) * gap) / bpi;
			if (tw < 1.f) tw = 1.f;
			for (int b = 0; b < bpi; b++) {
				float x = pad + b * (tw + gap);
				NVGcolor c = (b == cur) ? nvgRGB(0x2a, 0xc8, 0x66)
				           : (cur >= 0 && b < cur) ? NINJAM_GREEN
				           : (b == 0) ? nvgRGB(0xc8, 0x9a, 0x3a)
				           : nvgRGBA(0x00, 0x00, 0x00, 0x1a);
				nvgBeginPath(vg);
				nvgRoundedRect(vg, x, ty, tw, th, 2.f);
				nvgFillColor(vg, c);
				nvgFill(vg);
			}
			if (cur >= 0)
				drawTxt(vg, FONT_REG, pad, 58, 11.f, TH_TEXT_DIM,
					string::f("beat %d / %d", cur + 1, bpi), NVG_ALIGN_LEFT);
		}

		// Interval progress bar.
		const float by = 70.f, bh = 12.f;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, pad, by, avail, bh, 3.f);
		nvgFillColor(vg, nvgRGBA(0, 0, 0, 0x1e));
		nvgFill(vg);
		if (phase > 0.f) {
			nvgBeginPath(vg);
			nvgRoundedRect(vg, pad, by, avail * phase, bh, 3.f);
			nvgFillColor(vg, NINJAM_GREEN);
			nvgFill(vg);
		}

		// Roster (who's here), one per line.
		drawTxt(vg, FONT_BOLD, pad, 100, 11.f, TH_TEXT_DIM, "IN THE ROOM", NVG_ALIGN_LEFT);
		std::string who = module->rosterText();
		if (who.empty()) {
			drawTxt(vg, FONT_REG, pad, 116, 11.f, TH_TEXT_DIM, "(just you)", NVG_ALIGN_LEFT);
		} else {
			float y = 116.f;
			size_t start = 0;
			while (start < who.size() && y < box.size.y - 6) {
				size_t comma = who.find(", ", start);
				std::string name = who.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
				drawTxt(vg, FONT_REG, pad, y, 11.5f, nvgRGB(0x1c, 0x7a, 0x3e), name, NVG_ALIGN_LEFT, avail);
				y += 14.f;
				if (comma == std::string::npos) break;
				start = comma + 2;
			}
		}
	}
};

// Password text field — masks the displayed characters but keeps the real value in `text`.
struct NjPasswordField : TabField {
	void draw(const DrawArgs& args) override {
		std::string real = text;
		text = std::string(real.size(), '*');
		ui::TextField::draw(args);
		text = real;
	}
};

// "▾" button beside the server field: opens a menu of previously-joined servers.
struct ServerDropdownButton : OpaqueWidget {
	Ninjam* module = nullptr;
	ui::TextField* serverField = nullptr;
	bool hovered = false;
	void onEnter(const EnterEvent& e) override { hovered = true; OpaqueWidget::onEnter(e); }
	void onLeave(const LeaveEvent& e) override { hovered = false; OpaqueWidget::onLeave(e); }
	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && module) {
			ui::Menu* menu = createMenu();
			if (module->serverHistory.empty()) {
				menu->addChild(createMenuLabel("No previous servers"));
			} else {
				ui::TextField* sf = serverField;
				for (const std::string& hp : module->serverHistory)
					menu->addChild(createMenuItem(hp, "", [sf, hp]() { if (sf) sf->text = hp; }));
			}
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
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
struct JoinButton : OpaqueWidget {
	Ninjam* module = nullptr;
	ui::TextField* userField = nullptr;
	ui::TextField* passField = nullptr;
	ui::TextField* serverField = nullptr;
	bool hovered = false;
	void onEnter(const EnterEvent& e) override { hovered = true; OpaqueWidget::onEnter(e); }
	void onLeave(const LeaveEvent& e) override { hovered = false; OpaqueWidget::onLeave(e); }
	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			if (module && module->joined)
				module->stopAll();          // already connected -> disconnect
			else
				directJoin(module, userField, passField, serverField);
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
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
		box.size = Vec(width, 70);
		const float inner = 8.f;

		TabField* userField = new TabField;
		NjPasswordField* passField = new NjPasswordField;
		ServerField* serverField = new ServerField;

		float fieldW = (width - 2 * inner - 6) / 2.f;
		userField->box.pos = Vec(inner, 20);
		userField->box.size = Vec(fieldW, 18);
		userField->placeholder = "username";
		userField->text = module ? module->joinUser : "";
		addChild(userField);

		passField->box.pos = Vec(inner + fieldW + 6, 20);
		passField->box.size = Vec(fieldW, 18);
		passField->placeholder = "password";
		passField->text = module ? module->joinPass : "";
		addChild(passField);

		const float joinW = 46.f, dropW = 18.f, gap = 5.f;
		float serverW = width - 2 * inner - joinW - dropW - 2 * gap;
		serverField->box.pos = Vec(inner, 44);
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
		drop->box.pos = Vec(inner + serverW + gap, 44);
		drop->box.size = Vec(dropW, 18);
		addChild(drop);

		JoinButton* join = new JoinButton;
		join->module = module;
		join->userField = userField;
		join->passField = passField;
		join->serverField = serverField;
		join->box.pos = Vec(inner + serverW + gap + dropW + gap, 44);
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
		drawTxt(vg, FONT_BOLD, 8, 11, 11.5f, TH_TEXT, "Private server:", NVG_ALIGN_LEFT);
		Widget::draw(args);
	}
};

// Bottom I/O section: TRANSMIT IN row + peak meter + the two output rows. The jacks are
// added on top by NinjamWidget at matching x fractions / row y's. Label rows sit ~12 px
// above each jack row (IN ~26, audio ~68, CV ~104 in local coords).
struct OutputSection : Widget {
	Ninjam* module = nullptr;
	// L/R pairs grouped close together; the two stereo groups separated by a gap.
	static constexpr float xInL = 0.16f, xInR = 0.30f, xTx = 0.74f;          // transmit-in pair
	static constexpr float xMainL = 0.16f, xMainR = 0.30f, xPolyL = 0.62f, xPolyR = 0.76f; // out pairs
	static constexpr float xClick = 0.12f, xClock = 0.30f, xReset = 0.50f, xRun = 0.70f, xPhase = 0.88f;
	// A rounded jack-group box. `plate` = solid black output plate (white labels);
	// otherwise a light recessed well with a colored border (inputs).
	static void groupBox(NVGcontext* vg, float w, float xL, float xR, float top, float h,
	                     bool plate, NVGcolor border) {
		const float jh = 13.f, pad = 5.f;
		float x = w * xL - jh - pad, rx = w * xR + jh + pad;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, x, top, rx - x, h, 4.f);
		nvgFillColor(vg, plate ? TH_OUT_PLATE : TH_WELL);
		nvgFill(vg);
		nvgStrokeColor(vg, border);
		nvgStrokeWidth(vg, 1.f);
		nvgStroke(vg);
	}
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		const float w = box.size.x;
		// Plates are 44 tall: label near the top (~local+5 from plate top), jack centered
		// lower, leaving ~9 px of dark space below. Jack rows are 52 apart (set in the widget).

		// Label sits high in the plate; the jack is low (only ~4 px of plate below it),
		// matching Fundamental's OUT boxes. Jack local rows: IN 36, audio 88, CV 140.

		// INPUT group: light well, blue border, dark bold labels.
		groupBox(vg, w, xInL, xInR, 6, 46, false, TH_IN_BD);
		drawTxt(vg, FONT_BOLD, w * xInL - 18, 15, 11.f, TH_IN_BD, "IN", NVG_ALIGN_LEFT);
		drawTxt(vg, FONT_BOLD, w * xInL, 15, 11.f, TH_TEXT_DIM, "L", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, w * xInR, 15, 11.f, TH_TEXT_DIM, "R", NVG_ALIGN_CENTER);

		// Peak meter (thin bar in the gap between IN and OUT).
		const float bx = 12, by = 54, bw = w - 24, bh = 4;
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
		groupBox(vg, w, xMainL, xMainR, 58, 46, true, bd);
		groupBox(vg, w, xPolyL, xPolyR, 58, 46, true, bd);
		drawTxt(vg, FONT_BOLD, w * xMainL, 67, 11.f, TH_OUT_TEXT, "MAIN L", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, w * xMainR, 67, 11.f, TH_OUT_TEXT, "MAIN R", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, w * xPolyL, 67, 11.f, TH_OUT_TEXT, "PLY L", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, w * xPolyR, 67, 11.f, TH_OUT_TEXT, "PLY R", NVG_ALIGN_CENTER);

		groupBox(vg, w, xClick, xPhase, 110, 46, true, bd);
		drawTxt(vg, FONT_BOLD, w * xClick, 119, 9.5f, TH_OUT_TEXT, "CLICK", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, w * xClock, 119, 9.5f, TH_OUT_TEXT, "CLOCK", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, w * xReset, 119, 9.5f, TH_OUT_TEXT, "RESET", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, w * xRun, 119, 9.5f, TH_OUT_TEXT, "RUN", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, w * xPhase, 119, 9.5f, TH_OUT_TEXT, "PHASE", NVG_ALIGN_CENTER);
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
		led->onToggle = [module]() { if (module && module->isActive()) module->stopAll(); };
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
		card->box.pos = Vec(6, 50);
		addChild(card);
		joinCard = card;

		SearchField* s = new SearchField;
		s->box.pos = Vec(8, 128);
		s->box.size = Vec(W - 8 - 40, 20);
		addChild(s);
		search = s;

		RefreshButton* rb = new RefreshButton;
		rb->module = module;
		rb->box.pos = Vec(W - 36, 128);
		rb->box.size = Vec(28, 20);
		addChild(rb);
		refresh = rb;

		RoomBrowser* b = new RoomBrowser;
		b->module = module;
		b->search = s;
		b->box.pos = Vec(6, 150);
		b->box.size = Vec(W - 12, 44);
		addChild(b);
		browser = b;

		// ---- Connected: jam view (same region; replaces the connect UI) ----
		JamView* jv = new JamView;
		jv->module = module;
		jv->box.pos = Vec(6, 50);
		jv->box.size = Vec(W - 12, 144);
		addChild(jv);
		jamView = jv;

		// Bottom I/O section (always): TRANSMIT IN row + meter + output rows.
		const float oy = 200;
		OutputSection* out = new OutputSection;
		out->module = module;
		out->box.pos = Vec(0, oy);
		out->box.size = Vec(W, 162);
		addChild(out);

		const float rowIn = oy + 36, rowA = oy + 88, rowB = oy + 140; // 52px apart; low in plates
		// TRANSMIT IN (poly) + TX enable.
		addInput(createInputCentered<PJ301MPort>(Vec(W * OutputSection::xInL, rowIn), module, Ninjam::LEFT_INPUT));
		addInput(createInputCentered<PJ301MPort>(Vec(W * OutputSection::xInR, rowIn), module, Ninjam::RIGHT_INPUT));
		TxToggle* txBtn = new TxToggle;
		txBtn->module = module;
		txBtn->box.size = Vec(40, 18);
		txBtn->box.pos = Vec(W * OutputSection::xTx - 20, rowIn - 9);
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
	}

	// Swap connect UI <-> jam view each frame; keep our broadcast channels in sync.
	void step() override {
		bool joined = nj && nj->joined;
		if (joinCard) joinCard->visible = !joined;
		if (search) search->visible = !joined;
		if (refresh) refresh->visible = !joined;
		if (browser) browser->visible = !joined;
		if (jamView) jamView->visible = joined;
		if (metro) metro->visible = joined;
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
		menu->addChild(createBoolPtrMenuItem("Metronome click", "", &module->clickEnabled));

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
	}
};

Model* modelNinjam = createModel<Ninjam, NinjamWidget>("Ninjam");
