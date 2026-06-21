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
		INPUTS_LEN
	};
	enum OutputId {
		LEFT_OUTPUT,
		RIGHT_OUTPUT,
		CLICK_OUTPUT,
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

	// Declared last so it is destroyed FIRST: NjClient::~ joins its threads before the
	// state its callbacks touch (roster/atomics above) is torn down.
	akaudio::nj::NjClient njclient;

	Ninjam() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configOutput(LEFT_OUTPUT, "Left");
		configOutput(RIGHT_OUTPUT, "Right");
		configOutput(CLICK_OUTPUT, "Metronome click");
		float sr = APP->engine->getSampleRate();
		stream.setSampleRate(sr);
		njclient.setSampleRate(sr);
		directory.refresh();
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

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		stream.setSampleRate(e.sampleRate);
		njclient.setSampleRate(e.sampleRate);
	}

	void process(const ProcessArgs& args) override {
		float l = 0.f, r = 0.f;
		if (mode == MODE_JOIN)
			njclient.pull(l, r); // leaves l/r at 0 on underrun
		else
			stream.pull(l, r);
		// Audio in Rack is ±5V line level.
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
		float click = 0.f;
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
		} else {
			currentBeat.store(-1, std::memory_order_relaxed);
			beatPhase = 0.0;
			beatIndex = 0;
			clickEnv = 0.f;
		}
		outputs[CLICK_OUTPUT].setVoltage(click * 5.f);
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

static const NVGcolor NINJAM_GREEN = nvgRGB(0x3a, 0xd0, 0x6a);

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
			nvgFillColor(vg, active ? nvgRGBA(0x3a, 0xd0, 0x6a, 0x2e) : nvgRGBA(0xff, 0xff, 0xff, 0x12));
			nvgFill(vg);
		}
		if (active) {
			nvgBeginPath(vg);
			nvgRoundedRect(vg, 2, 3, 3, h - 6, 1.5);
			nvgFillColor(vg, NINJAM_GREEN);
			nvgFill(vg);
		}

		// Name (clipped before the icons).
		drawTxt(vg, FONT_BOLD, pad, ICON_CY, 12.5f,
			active ? nvgRGB(0x8a, 0xf0, 0xaa) : nvgRGB(0xe6, 0xea, 0xee),
			room.name, NVG_ALIGN_LEFT, w - pad - 52);

		// Listen (speaker) + Join (enter) icons.
		const NVGcolor dim = nvgRGBA(0xff, 0xff, 0xff, 0x44);
		const NVGcolor off = nvgRGBA(0xff, 0xff, 0xff, 0x1c);
		const NVGcolor bright = nvgRGB(0xe6, 0xea, 0xee);
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
		drawTxt(vg, FONT_REG, pad, 26, 9.5f, nvgRGB(0x8a, 0x97, 0xa3), stats, NVG_ALIGN_LEFT, w - 2 * pad);

		// Players line (if anyone is in the room).
		if (!room.users.empty()) {
			std::string who;
			for (size_t i = 0; i < room.users.size(); i++) {
				if (i)
					who += ", ";
				who += room.users[i];
			}
			drawTxt(vg, FONT_REG, pad, 39, 8.5f, nvgRGB(0x5f, 0xb0, 0x77), who, NVG_ALIGN_LEFT, w - 2 * pad);
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
		nvgFillColor(args.vg, nvgRGBA(0, 0, 0, 0x40));
		nvgFill(args.vg);
		ui::ScrollWidget::draw(args);
		if (container->children.empty()) {
			std::string msg = (module && module->directory.loading())
				? "Loading rooms\xe2\x80\xa6"
				: "No rooms \xe2\x80\x94 try Refresh";
			drawTxt(args.vg, FONT_REG, box.size.x / 2, box.size.y / 2, 11.f,
				nvgRGB(0x6a, 0x77, 0x83), msg, NVG_ALIGN_CENTER);
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
		nvgFillColor(vg, hovered ? nvgRGBA(0xff, 0xff, 0xff, 0x1c) : nvgRGBA(0xff, 0xff, 0xff, 0x0e));
		nvgFill(vg);
		bool loading = module && module->directory.loading();
		drawTxt(vg, FONT_REG, box.size.x / 2, box.size.y / 2, 14.f,
			loading ? nvgRGBA(0x3a, 0xd0, 0x6a, 0x88) : nvgRGB(0xcf, 0xd8, 0xe0),
			"\xe2\x86\xbb", NVG_ALIGN_CENTER); // ↻
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
		             : hovered ? nvgRGB(0xe6, 0xea, 0xee) : nvgRGBA(0xff, 0xff, 0xff, 0x55);
		drawMetronomeIcon(args.vg, box.size.x / 2, box.size.y / 2, std::min(box.size.x, box.size.y), col);
	}
};

// Top transport block: connection/tempo status (or directory status), with a row of
// per-beat ticks under it when joined (elapsed lit, current brightest, downbeat accented).
struct TransportBlock : Widget {
	Ninjam* module = nullptr;
	void draw(const DrawArgs& args) override {
		if (!module)
			return;
		NVGcontext* vg = args.vg;
		const float w = box.size.x, h = box.size.y;

		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, w, h, 4);
		nvgFillColor(vg, nvgRGBA(0, 0, 0, 0x40));
		nvgFill(vg);

		std::string jam = module->jamStatusText();
		const bool joined = !jam.empty();
		// Line 1: status text (leave room on the right for the metronome + LED).
		drawTxt(vg, FONT_REG, 8, 13, 9.5f,
			joined ? NINJAM_GREEN : nvgRGB(0x7a, 0x86, 0x92),
			joined ? jam : module->directory.status(), NVG_ALIGN_LEFT, w - 8 - 44);

		// Line 2: beat ticks (only when joined to a tempo).
		int bpi = module->jamBpi.load(std::memory_order_relaxed);
		int cur = module->currentBeat.load(std::memory_order_relaxed);
		if (joined && bpi > 0) {
			const float pad = 8.f, ty = h - 11.f, th = 7.f, gap = 2.f;
			float avail = w - 2 * pad;
			float tw = (avail - (bpi - 1) * gap) / bpi;
			if (tw < 1.f) tw = 1.f;
			for (int b = 0; b < bpi; b++) {
				float x = pad + b * (tw + gap);
				NVGcolor c = (b == cur) ? nvgRGB(0x7a, 0xf0, 0xa0)               // current: brightest
				           : (cur >= 0 && b < cur) ? NINJAM_GREEN                // elapsed: green
				           : (b == 0) ? nvgRGB(0xc8, 0x9a, 0x3a)                 // downbeat (upcoming): amber
				           : nvgRGBA(0xff, 0xff, 0xff, 0x22);                     // upcoming: dim
				nvgBeginPath(vg);
				nvgRoundedRect(vg, x, ty, tw, th, 1.5f);
				nvgFillColor(vg, c);
				nvgFill(vg);
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
		nvgFillColor(args.vg, hovered ? nvgRGBA(0xff, 0xff, 0xff, 0x1c) : nvgRGBA(0xff, 0xff, 0xff, 0x0e));
		nvgFill(args.vg);
		drawTxt(args.vg, FONT_REG, box.size.x / 2, box.size.y / 2, 11.f,
			nvgRGB(0xcf, 0xd8, 0xe0), "\xe2\x96\xbe", NVG_ALIGN_CENTER); // ▾
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
		// Green = JOIN (connect); amber/red = LEAVE (disconnect).
		nvgFillColor(args.vg, connected
			? (hovered ? nvgRGBA(0xe0, 0x6a, 0x3a, 0x88) : nvgRGBA(0xe0, 0x6a, 0x3a, 0x55))
			: (hovered ? nvgRGBA(0x3a, 0xd0, 0x6a, 0x66) : nvgRGBA(0x3a, 0xd0, 0x6a, 0x3a)));
		nvgFill(args.vg);
		drawTxt(args.vg, FONT_BOLD, box.size.x / 2, box.size.y / 2, 10.f,
			connected ? nvgRGB(0xff, 0xe4, 0xd6) : nvgRGB(0xdf, 0xf6, 0xe6),
			connected ? "LEAVE" : "JOIN", NVG_ALIGN_CENTER);
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
		nvgFillColor(vg, nvgRGBA(0xff, 0xff, 0xff, 0x08));
		nvgFill(vg);
		drawTxt(vg, FONT_BOLD, 8, 11, 9.5f, nvgRGB(0xb6, 0xc0, 0xca), "Private server:", NVG_ALIGN_LEFT);
		Widget::draw(args);
	}
};

// Footer output level meter (peak with fast release) + divider.
struct MeterWidget : Widget {
	Ninjam* module = nullptr;
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		const float w = box.size.x;
		nvgBeginPath(vg);
		nvgMoveTo(vg, 10, 0.5f);
		nvgLineTo(vg, w - 10, 0.5f);
		nvgStrokeColor(vg, nvgRGBA(0xff, 0xff, 0xff, 0x18));
		nvgStroke(vg);

		const float bx = 12, by = 8, bw = w - 24, bh = 7;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, bx, by, bw, bh, 3);
		nvgFillColor(vg, nvgRGBA(0, 0, 0, 0x66));
		nvgFill(vg);

		float p = module ? module->peak.load(std::memory_order_relaxed) : 0.f;
		p = std::max(0.f, std::min(1.f, p));
		if (p > 0.001f) {
			NVGcolor c = p > 0.95f ? nvgRGB(0xe0, 0x4a, 0x3a)
			           : p > 0.80f ? nvgRGB(0xe0, 0xc0, 0x3a)
			                       : NINJAM_GREEN;
			nvgBeginPath(vg);
			nvgRoundedRect(vg, bx, by, bw * p, bh, 3);
			nvgFillColor(vg, c);
			nvgFill(vg);
		}
		drawTxt(vg, FONT_REG, bx, by + bh + 9, 7.5f, nvgRGB(0x6a, 0x77, 0x83), "OUTPUT", NVG_ALIGN_LEFT);

		// Labels under the L / R / CLICK jacks (jacks are centered at these x, drawn on top).
		const float ly = box.size.y - 3.f;
		drawTxt(vg, FONT_REG, w * 0.28f, ly, 7.5f, nvgRGB(0x8a, 0x97, 0xa3), "L", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_REG, w * 0.50f, ly, 7.5f, nvgRGB(0x8a, 0x97, 0xa3), "R", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_REG, w * 0.72f, ly, 7.5f, nvgRGB(0x8a, 0x97, 0xa3), "CLICK", NVG_ALIGN_CENTER);
	}
};

struct NinjamWidget : ModuleWidget {
	NinjamWidget(Ninjam* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Ninjam.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		const float W = box.size.x;

		// Top transport block: status + beat ticks. Metronome toggle + LED sit on its
		// top-right, drawn over the block.
		TransportBlock* transport = new TransportBlock;
		transport->module = module;
		transport->box.pos = Vec(6, 16);
		transport->box.size = Vec(W - 12, 40);
		addChild(transport);

		// Clickable status LED (top-right): green=live, amber=connecting, red=stopped.
		// Click stops the active source.
		ClickableLed* led = new ClickableLed;
		led->box.size = Vec(13, 13);
		led->box.pos = Vec(W - 6 - 4 - 13, 22);
		led->isLive = [module]() { return module && module->ledLive(); };
		led->isPending = [module]() { return module && module->ledPending(); };
		led->onToggle = [module]() { if (module && module->isActive()) module->stopAll(); };
		addChild(led);

		// Metronome click toggle, just left of the LED.
		MetronomeToggle* metro = new MetronomeToggle;
		metro->module = module;
		metro->box.size = Vec(16, 16);
		metro->box.pos = Vec(W - 6 - 4 - 13 - 6 - 16, 20);
		addChild(metro);

		// Private-server direct-join card.
		JoinCard* joinCard = new JoinCard(module, W - 12);
		joinCard->box.pos = Vec(6, 62);
		addChild(joinCard);

		// Filter + refresh, right above the room list.
		SearchField* search = new SearchField;
		search->box.pos = Vec(8, 140);
		search->box.size = Vec(W - 8 - 40, 20);
		addChild(search);

		RefreshButton* refresh = new RefreshButton;
		refresh->module = module;
		refresh->box.pos = Vec(W - 36, 140);
		refresh->box.size = Vec(28, 20);
		addChild(refresh);

		RoomBrowser* browser = new RoomBrowser;
		browser->module = module;
		browser->search = search;
		browser->box.pos = Vec(6, 164);
		browser->box.size = Vec(W - 12, 160);
		addChild(browser);

		MeterWidget* meter = new MeterWidget;
		meter->module = module;
		meter->box.pos = Vec(0, 326);
		meter->box.size = Vec(W, 54); // spans down past the jacks so labels sit under them
		addChild(meter);

		// Outputs: L, R, CLICK.
		addOutput(createOutputCentered<PJ301MPort>(Vec(W * 0.28f, 360), module, Ninjam::LEFT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(Vec(W * 0.50f, 360), module, Ninjam::RIGHT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(Vec(W * 0.72f, 360), module, Ninjam::CLICK_OUTPUT));
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
	}
};

Model* modelNinjam = createModel<Ninjam, NinjamWidget>("Ninjam");
