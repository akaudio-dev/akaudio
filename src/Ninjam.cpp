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
		float sr = APP->engine->getSampleRate();
		stream.setSampleRate(sr);
		njclient.setSampleRate(sr);
		directory.refresh();
	}

	// NjClient callbacks (fire on its background thread). Keep them light.
	akaudio::nj::NjClient::Callbacks jamCallbacks() {
		akaudio::nj::NjClient::Callbacks cb;
		cb.onConfig = [this](int bpm, int bpi) {
			jamBpm.store(bpm, std::memory_order_relaxed);
			jamBpi.store(bpi, std::memory_order_relaxed);
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

	// Pick a room (explicit click = start). Branches on the current mode.
	void selectRoom(const akaudio::Room& room) {
		stopAll();
		roomLabel = room.name.empty() ? room.host : room.name;
		if (mode == MODE_JOIN) {
			joinHost = room.host;
			joinPort = room.port;
			joinStart();
		} else {
			url = room.playUrl();
			listen();
		}
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
		njclient.start(joinHost, joinPort, joinUser, joinPass, jamCallbacks());
		joined = true;
	}

	bool isActive() const { return mode == MODE_JOIN ? joined : listening; }

	void toggleActive() {
		if (isActive())
			stopAll();
		else if (mode == MODE_JOIN)
			joinStart();
		else
			listen();
	}

	// True if the given room is the one currently playing (mode-aware).
	bool isActiveRoom(const akaudio::Room& room) const {
		if (mode == MODE_JOIN)
			return joined && joinHost == room.host && joinPort == room.port;
		return listening && url == room.playUrl();
	}

	// Row click: stop if it's the active room, otherwise switch to it.
	void toggleRoom(const akaudio::Room& room) {
		if (isActiveRoom(room))
			stopAll();
		else
			selectRoom(room);
	}

	// LED state helpers (mode-aware).
	bool ledLive() const {
		return mode == MODE_JOIN
			? njclient.state() == akaudio::nj::NjClient::State::Connected
			: stream.getState() == akaudio::StreamClient::State::Playing;
	}
	bool ledPending() const {
		return mode == MODE_JOIN ? (joined && !ledLive())
		                         : (listening && stream.getState() != akaudio::StreamClient::State::Playing);
	}

	// A room is selectable in the browser if it can be used in the current mode:
	// JOIN needs host/port; LISTEN needs an Icecast stream URL.
	bool roomSelectable(const akaudio::Room& room) const {
		return mode == MODE_JOIN ? (!room.host.empty() && room.port > 0) : room.playable();
	}

	// One-line jam status for the panel/menu when in JOIN mode (empty otherwise).
	std::string jamStatusText() {
		if (mode != MODE_JOIN || !joined)
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

// Editable jam-stream URL field shown in the context menu (LISTEN mode).
struct NinjamUrlField : ui::TextField {
	Ninjam* module = nullptr;
	void onAction(const ActionEvent& e) override {
		if (module) {
			module->url = text;
			module->roomLabel = "Custom";
		}
		ui::TextField::onAction(e);
	}
};

// "host:port" field for a manual protocol join (JOIN mode). Enter sets the target
// and connects (anonymous unless a username/password is also set).
struct JoinServerField : ui::TextField {
	Ninjam* module = nullptr;
	void onAction(const ActionEvent& e) override {
		if (module) {
			std::string s = text;
			size_t colon = s.rfind(':');
			if (colon != std::string::npos) {
				module->joinHost = s.substr(0, colon);
				module->joinPort = std::atoi(s.substr(colon + 1).c_str());
			} else {
				module->joinHost = s;
			}
			if (module->joinPort <= 0)
				module->joinPort = 2049;
			module->roomLabel = module->joinHost;
			module->stopAll();
			module->joinStart();
		}
		ui::TextField::onAction(e);
	}
};

struct JoinUserField : ui::TextField {
	Ninjam* module = nullptr;
	void onAction(const ActionEvent& e) override {
		if (module) module->joinUser = text;
		ui::TextField::onAction(e);
	}
};

struct JoinPassField : ui::TextField {
	Ninjam* module = nullptr;
	void onAction(const ActionEvent& e) override {
		if (module) module->joinPass = text;
		ui::TextField::onAction(e);
	}
};

// Populate a menu with the cached room list. Kicks a background refresh first so
// the data freshens for next time, but always renders instantly from the cache —
// it never blocks the UI thread on the network.
static void appendRoomMenu(Menu* menu, Ninjam* module) {
	module->directory.refresh();

	menu->addChild(createMenuLabel(module->directory.status()));
	menu->addChild(createMenuItem("Refresh rooms", "", [module]() {
		module->directory.refresh();
	}));

	std::vector<akaudio::Room> rooms = module->directory.rooms();
	menu->addChild(new MenuSeparator);

	int shown = 0;
	for (const akaudio::Room& room : rooms) {
		if (!module->roomSelectable(room))
			continue;
		std::string cap = room.userMax > 0
			? string::f("%d/%d", room.userCount, room.userMax)
			: string::f("%d", room.userCount);
		std::string label = room.name + "   " + cap + " \xe2\x80\xa2 " + string::f("%d BPM", room.bpm);
		menu->addChild(createCheckMenuItem(label, "",
			[module, room]() { return module->isActiveRoom(room); },
			[module, room]() { module->selectRoom(room); }));
		shown++;
	}
	if (shown == 0)
		menu->addChild(createMenuLabel("No playable rooms — try Refresh"));
}

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

// One room in the list. Click toggles listening to it.
struct RoomRow : OpaqueWidget {
	Ninjam* module = nullptr;
	akaudio::Room room;
	bool hovered = false;

	void onEnter(const EnterEvent& e) override {
		hovered = true;
		OpaqueWidget::onEnter(e);
	}
	void onLeave(const LeaveEvent& e) override {
		hovered = false;
		OpaqueWidget::onLeave(e);
	}
	void onButton(const ButtonEvent& e) override {
		// Act before the base class: OpaqueWidget::onButton consumes the event.
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			if (module)
				module->toggleRoom(room);
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		const float w = box.size.x, h = box.size.y, pad = 10.f;
		const bool active = module && module->isActiveRoom(room);

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

		// Name + listening glyph.
		drawTxt(vg, FONT_BOLD, pad, 13, 12.5f,
			active ? nvgRGB(0x8a, 0xf0, 0xaa) : nvgRGB(0xe6, 0xea, 0xee),
			room.name, NVG_ALIGN_LEFT, w - pad - 24);
		drawTxt(vg, FONT_REG, w - 15, 13, 11.f,
			active ? NINJAM_GREEN : nvgRGBA(0xff, 0xff, 0xff, 0x44),
			active ? "\xe2\x96\xa0" : "\xe2\x96\xb6", NVG_ALIGN_CENTER); // ■ / ▶

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
			if (!module->roomSelectable(room))
				continue; // LISTEN: http MP3 mount; JOIN: any room with host/port
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

// Panel header (title + accent rule).
struct HeaderWidget : Widget {
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		drawTxt(vg, FONT_BOLD, 10, box.size.y / 2, 16.f, nvgRGB(0xf2, 0xf5, 0xf8), "NINJAM");
		drawTxt(vg, FONT_REG, 74, box.size.y / 2 + 1, 8.5f, NINJAM_GREEN, "JAM RADIO");
		nvgBeginPath(vg);
		nvgMoveTo(vg, 10, box.size.y - 1);
		nvgLineTo(vg, box.size.x - 10, box.size.y - 1);
		nvgStrokeColor(vg, nvgRGBA(0x3a, 0xd0, 0x6a, 0x80));
		nvgStrokeWidth(vg, 1.f);
		nvgStroke(vg);
	}
};

// Status line: the jam connection (when joined via protocol) else the directory status.
struct StatusLabel : Widget {
	Ninjam* module = nullptr;
	void draw(const DrawArgs& args) override {
		if (!module)
			return;
		std::string jam = module->jamStatusText();
		bool isJam = !jam.empty();
		drawTxt(args.vg, FONT_REG, 0, box.size.y / 2, 9.f,
			isJam ? NINJAM_GREEN : nvgRGB(0x7a, 0x86, 0x92),
			isJam ? jam : module->directory.status(), NVG_ALIGN_LEFT, box.size.x);
	}
};

// Two-segment mode switch in the header: LISTEN (Icecast) vs JOIN (protocol).
struct ModeToggle : OpaqueWidget {
	Ninjam* module = nullptr;
	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			if (module) {
				float midx = box.size.x / 2.f;
				module->setMode(e.pos.x < midx ? Ninjam::MODE_LISTEN : Ninjam::MODE_JOIN);
			}
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
	}
	void seg(NVGcontext* vg, float x, float w, const char* label, bool on) {
		nvgBeginPath(vg);
		nvgRoundedRect(vg, x, 0, w, box.size.y, 3);
		nvgFillColor(vg, on ? nvgRGBA(0x3a, 0xd0, 0x6a, 0x40) : nvgRGBA(0xff, 0xff, 0xff, 0x0e));
		nvgFill(vg);
		drawTxt(vg, FONT_BOLD, x + w / 2, box.size.y / 2, 8.5f,
			on ? nvgRGB(0xaf, 0xf2, 0xc6) : nvgRGB(0x8a, 0x97, 0xa3), label, NVG_ALIGN_CENTER);
	}
	void draw(const DrawArgs& args) override {
		if (!module)
			return;
		float h = box.size.x / 2.f;
		seg(args.vg, 0, h - 1, "LISTEN", module->mode == Ninjam::MODE_LISTEN);
		seg(args.vg, h + 1, h - 1, "JOIN", module->mode == Ninjam::MODE_JOIN);
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

		HeaderWidget* header = new HeaderWidget;
		header->box.pos = Vec(0, 14);
		header->box.size = Vec(W, 30);
		addChild(header);

		// Clickable status LED: green=live, amber=connecting, red=stopped. Click
		// toggles the active source (mode-aware: Icecast stream or protocol join).
		ClickableLed* led = new ClickableLed;
		led->box.size = Vec(13, 13);
		led->box.pos = Vec(W - 13, 24).minus(led->box.size.div(2));
		led->isLive = [module]() { return module && module->ledLive(); };
		led->isPending = [module]() { return module && module->ledPending(); };
		led->onToggle = [module]() { if (module) module->toggleActive(); };
		addChild(led);

		// LISTEN / JOIN mode switch in the header.
		ModeToggle* modeToggle = new ModeToggle;
		modeToggle->module = module;
		modeToggle->box.size = Vec(104, 15);
		modeToggle->box.pos = Vec(W - 22 - 104, 16);
		addChild(modeToggle);

		SearchField* search = new SearchField;
		search->box.pos = Vec(8, 48);
		search->box.size = Vec(W - 8 - 40, 20);
		addChild(search);

		RefreshButton* refresh = new RefreshButton;
		refresh->module = module;
		refresh->box.pos = Vec(W - 36, 48);
		refresh->box.size = Vec(28, 20);
		addChild(refresh);

		StatusLabel* status = new StatusLabel;
		status->module = module;
		status->box.pos = Vec(12, 71);
		status->box.size = Vec(W - 24, 12);
		addChild(status);

		RoomBrowser* browser = new RoomBrowser;
		browser->module = module;
		browser->search = search;
		browser->box.pos = Vec(6, 86);
		browser->box.size = Vec(W - 12, 240);
		addChild(browser);

		MeterWidget* meter = new MeterWidget;
		meter->module = module;
		meter->box.pos = Vec(0, 330);
		meter->box.size = Vec(W, 50);
		addChild(meter);

		addOutput(createOutputCentered<PJ301MPort>(Vec(W * 0.62f, 360), module, Ninjam::LEFT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(Vec(W * 0.80f, 360), module, Ninjam::RIGHT_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Ninjam* module = getModule<Ninjam>();
		if (!module)
			return;

		menu->addChild(new MenuSeparator);

		// Mode: Icecast listen vs NINJAM protocol join.
		menu->addChild(createMenuLabel("Mode"));
		menu->addChild(createCheckMenuItem("Listen (Icecast stream)", "",
			[module]() { return module->mode == Ninjam::MODE_LISTEN; },
			[module]() { module->setMode(Ninjam::MODE_LISTEN); }));
		menu->addChild(createCheckMenuItem("Join (NINJAM protocol)", "",
			[module]() { return module->mode == Ninjam::MODE_JOIN; },
			[module]() { module->setMode(Ninjam::MODE_JOIN); }));

		menu->addChild(new MenuSeparator);
		if (module->mode == Ninjam::MODE_JOIN) {
			std::string jam = module->jamStatusText();
			menu->addChild(createMenuLabel(jam.empty() ? "Not joined" : jam));
		} else {
			menu->addChild(createMenuLabel("Status: " + module->stream.getStatusText()));
		}
		menu->addChild(createMenuItem(module->isActive() ? "Stop" : "Start", "", [module]() {
			module->toggleActive();
		}));

		menu->addChild(new MenuSeparator);
		menu->addChild(createSubmenuItem("Public rooms", module->roomLabel, [module](Menu* sub) {
			appendRoomMenu(sub, module);
		}));

		if (module->mode == Ninjam::MODE_JOIN) {
			menu->addChild(createMenuLabel("Join server (host:port):"));
			JoinServerField* server = new JoinServerField;
			server->box.size.x = 260;
			server->module = module;
			server->text = module->joinHost.empty() ? "" : (module->joinHost + ":" + std::to_string(module->joinPort));
			server->placeholder = "host:2049";
			menu->addChild(server);

			menu->addChild(createMenuLabel("Username (blank = anonymous):"));
			JoinUserField* userField = new JoinUserField;
			userField->box.size.x = 260;
			userField->module = module;
			userField->text = module->joinUser;
			userField->placeholder = "display name";
			menu->addChild(userField);

			menu->addChild(createMenuLabel("Password (blank = anonymous):"));
			JoinPassField* passField = new JoinPassField;
			passField->box.size.x = 260;
			passField->module = module;
			passField->text = module->joinPass;
			passField->placeholder = "(only for registered/private servers)";
			menu->addChild(passField);
		} else {
			menu->addChild(createMenuLabel("Jam stream URL (http:// Icecast MP3):"));
			NinjamUrlField* field = new NinjamUrlField;
			field->box.size.x = 260;
			field->module = module;
			field->text = module->url;
			field->placeholder = "http://host:port/room";
			menu->addChild(field);
		}
	}
};

Model* modelNinjam = createModel<Ninjam, NinjamWidget>("Ninjam");
