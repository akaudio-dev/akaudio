// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

// Recorder — a Ninjam expander that saves the jam. It has NO audio path and reads no
// clock: it is a control panel for Ninjam's wire archive (docs/LOOPER_DESIGN.md §7),
// which writes every received per-player interval and our transmitted mix to disk as
// the raw OGG bytes they already are (per-player .ogg files + a JSON-lines index on
// the session timeline) — so a DAW can later reassemble the whole jam. All the work is
// in Ninjam/NjArchive; this module is the REC arm, the "record own TX" toggle, and the
// live per-player status. It reaches Ninjam through the RecorderLink interface by
// dynamic_cast on its adjacent module — no expander messages, no cables.

#include "plugin.hpp"
#include "Theme.hpp"
#include "RecorderLink.hpp"
#include "JamExport.hpp"

#include <osdialog.h>

#include <atomic>
#include <thread>

namespace {

inline NVGcolor rcText()    { return akTheme(nvgRGB(0x24, 0x27, 0x2b), nvgRGB(0xed, 0xed, 0xed)); }
inline NVGcolor rcTextDim() { return akTheme(nvgRGB(0x5c, 0x61, 0x68), nvgRGB(0x9a, 0xa0, 0xa6)); }
inline NVGcolor rcRed()     { return akTheme(nvgRGB(0xc0, 0x39, 0x2b), nvgRGB(0xe0, 0x60, 0x52)); }
inline NVGcolor rcWell()    { return akShade(akDark() ? 0x14 : 0x10); }
inline NVGcolor rcBorder()  { return akShade(akDark() ? 0x2e : 0x26); }

// The auto-export runs on its own thread: exportJamAls scans every archived OGG and
// gzips a multi-MB document — synchronous in step() it froze the whole Rack UI at the
// moment recording stops. Self-contained (reads disk, logs) — it never touches the
// module, so module removal mid-export is safe. One at a time.
std::atomic<bool> alsExportBusy{false};
void launchAutoExport(const std::string& jamRoot, bool liteMode) {
	bool expected = false;
	if (!alsExportBusy.compare_exchange_strong(expected, true)) {
		WARN("akaudio: .als export already running, skipping %s", jamRoot.c_str());
		return;
	}
	std::thread([jamRoot, liteMode]() {
		std::string whyNot;
		std::string out = akaudio::exportJamAls(jamRoot, liteMode, &whyNot);
		if (!out.empty())
			INFO("akaudio: exported Live set %s", out.c_str());
		else
			WARN("akaudio: auto .als export failed: %s", whyNot.c_str());
		alsExportBusy.store(false);
	}).detach();
}

} // namespace

struct Recorder : Module {
	enum ParamId { REC_PARAM, TX_PARAM, PARAMS_LEN };
	enum InputId { INPUTS_LEN };
	enum OutputId { OUTPUTS_LEN };
	enum LightId { REC_LIGHT, LIGHTS_LEN };

	// Reconcile state (UI thread): the arm/recordTx truth lives in the adjacent Ninjam
	// (RecorderLink); these latch params mirror it so the buttons are MIDI-mappable.
	bool prevRec = false, prevTx = true;
	// The folder new session dirs are created in. Owned + persisted here; pushed to the
	// adjacent Ninjam (which builds the dated session path) every UI step.
	std::string sessionBase = akaudio::defaultJamsDir();
	// The jam root of the current/most recent recording (<base>/<stamp>_<room>), cached
	// while the archive runs — the link's session name is stamped fresh on every arm, and
	// the Ninjam module may be gone by the time we want to export. Not persisted.
	std::string lastJamRoot;
	// Export the .als automatically when recording stops (menu-toggleable, persisted).
	bool autoExportAls = true;
	// Target Ableton Live edition for the export (persisted). Lite's 8-track cap gets
	// the reduced flavor: 6 grid tracks + one merged players lane + TX, no clones.
	bool liteExport = false;
	// Auto-export fires this many wall-clock seconds after the disarm, so the Looper's
	// worker can finish encoding the boundary take (frames would be display-rate-
	// dependent). < 0 = no export pending.
	double exportAt = -1.0;
	bool prevActive = false;

	Recorder() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configSwitch(REC_PARAM, 0.f, 1.f, 0.f, "Record (arm the wire archive)", {"Idle", "Armed"});
		configSwitch(TX_PARAM, 0.f, 1.f, 1.f, "Also record your transmitted mix", {"No", "Yes"});
	}

	// The adjacent Ninjam (either side) as a RecorderLink. UI thread only (expander
	// pointers are updated by the engine between blocks; module removal is UI-thread).
	akaudio::RecorderLink* link() {
		Module* l = leftExpander.module;
		if (l && l->model == modelNinjam) return dynamic_cast<akaudio::RecorderLink*>(l);
		Module* r = rightExpander.module;
		if (r && r->model == modelNinjam) return dynamic_cast<akaudio::RecorderLink*>(r);
		return nullptr;
	}

	// Buttons are handled in the widget step (UI thread), not process(), since they act
	// on the RecorderLink (thread ops). process() does nothing — no audio path.
	void process(const ProcessArgs& args) override {}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "sessionBase", json_string(akaudio::collapseHome(sessionBase).c_str()));
		json_object_set_new(root, "autoExportAls", json_boolean(autoExportAls));
		json_object_set_new(root, "liteExport", json_boolean(liteExport));
		return root;
	}
	void dataFromJson(json_t* root) override {
		json_t* j = json_object_get(root, "sessionBase");
		if (j && json_string_value(j) && *json_string_value(j))
			sessionBase = akaudio::expandHome(json_string_value(j));
		if (json_t* a = json_object_get(root, "autoExportAls"))
			autoExportAls = json_is_true(a);
		if (json_t* le = json_object_get(root, "liteExport"))
			liteExport = json_is_true(le);
		// REC never persists armed: a loaded patch must come up NOT recording (Rack
		// restores the param before this runs, so force it back off here). The arm truth
		// lives in Ninjam and defaults off anyway; this stops the reconcile re-arming it.
		params[REC_PARAM].setValue(0.f);
		prevRec = false;
	}
};

// ------------------------------------------------------------------------------------

// A raised pill toggle: "REC TX" with an LED dot (red when your TX is also archived).
struct TxToggleButton : app::Switch {
	Recorder* rec = nullptr;
	bool hovered = false;
	void onEnter(const EnterEvent& e) override { hovered = true; app::Switch::onEnter(e); }
	void onLeave(const LeaveEvent& e) override { hovered = false; app::Switch::onLeave(e); }
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		const akaudio::RecorderLink* lk = rec ? rec->link() : nullptr;
		bool on = lk && lk->recordOwnTx();
		const float w = box.size.x, h = box.size.y, r = h / 2.f;
		NVGcolor body = akTheme(nvgRGB(0xb9, 0xbd, 0xc2), nvgRGB(0x44, 0x48, 0x4c));
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, w, h, r);
		nvgFillColor(vg, hovered ? akTheme(nvgRGB(0xa8,0xac,0xb2), nvgRGB(0x52,0x56,0x5b)) : body);
		nvgFill(vg);
		// Soft top highlight (raised look, like the Looper's scene/stop buttons).
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 1.f, 1.f, w - 2.f, h / 2.f, r);
		nvgFillColor(vg, akDark() ? nvgRGBA(0xff,0xff,0xff,0x10) : nvgRGBA(0xff,0xff,0xff,0x55));
		nvgFill(vg);
		// LED dot (left) + label.
		float cy = h / 2.f;
		nvgBeginPath(vg);
		nvgCircle(vg, 8.f, cy, 3.f);
		nvgFillColor(vg, on ? rcRed() : akShade(0x40));
		nvgFill(vg);
		drawTxt(vg, FONT_BOLD, 15.f, cy, 8.f, rcText(), "REC TX", NVG_ALIGN_LEFT);
	}
};

// Status panel: a clear state badge, a session summary, and one row per source.
struct RecStatusView : Widget {
	Recorder* rec = nullptr;
	static std::string humanSize(long b) {
		char c[24];
		if (b >= 1024L * 1024L) (void) std::snprintf(c, sizeof(c), "%.1f MB", b / (1024.0 * 1024.0));
		else if (b >= 1024L)    (void) std::snprintf(c, sizeof(c), "%ld KB", b / 1024L);
		else                    (void) std::snprintf(c, sizeof(c), "%ld B", b);
		return c;
	}
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		const float w = box.size.x;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, w, box.size.y, 4.f);
		nvgFillColor(vg, rcWell());
		nvgFill(vg);
		nvgStrokeColor(vg, rcBorder());
		nvgStroke(vg);

		const akaudio::RecorderLink* lk = rec ? rec->link() : nullptr;
		float y = 13.f;
		if (!lk) {
			drawTxt(vg, FONT_BOLD, w / 2, box.size.y / 2 - 6, 8.5f, rcTextDim(), "Place next", NVG_ALIGN_CENTER, w - 8);
			drawTxt(vg, FONT_BOLD, w / 2, box.size.y / 2 + 6, 8.5f, rcTextDim(), "to Ninjam", NVG_ALIGN_CENTER, w - 8);
			return;
		}
		bool active = lk->recActive();
		bool armed = lk->recArmed();
		// State badge: a filled dot + word (red while writing, amber armed, grey idle).
		NVGcolor sc = active ? rcRed() : (armed ? akTheme(nvgRGB(0xd9,0x8b,0x1a), nvgRGB(0xf0,0xb0,0x40)) : rcTextDim());
		const char* word = active ? "RECORDING" : (armed ? (lk->recJoined() ? "ARMED" : "WAITING") : "IDLE");
		nvgBeginPath(vg); nvgCircle(vg, 9, y, 3.f); nvgFillColor(vg, sc); nvgFill(vg);
		drawTxt(vg, FONT_BOLD, 16, y, 9.f, sc, word, NVG_ALIGN_LEFT, w - 20);
		y += 15.f;
		// Summary: intervals + total size (or a "→ folder" hint when nothing yet).
		long iv = lk->recIntervals();
		if (iv > 0) {
			char sum[48];
			(void) std::snprintf(sum, sizeof(sum), "%ld interval%s", iv, iv == 1 ? "" : "s");
			drawTxt(vg, FONT_REG, 8, y, 8.5f, rcText(), sum, NVG_ALIGN_LEFT, w - 16);
			y += 12.f;
			drawTxt(vg, FONT_REG, 8, y, 8.5f, rcTextDim(), humanSize(lk->recBytes()), NVG_ALIGN_LEFT, w - 16);
			y += 14.f;
		} else {
			drawTxt(vg, FONT_REG, 8, y, 8.f, rcTextDim(), "nothing yet", NVG_ALIGN_LEFT, w - 12);
			y += 14.f;
		}
		auto rows = lk->recStatus();
		if (rows.empty())
			return;
		nvgBeginPath(vg); nvgMoveTo(vg, 8, y - 5.f); nvgLineTo(vg, w - 8, y - 5.f);
		nvgStrokeColor(vg, rcBorder()); nvgStroke(vg);
		for (const auto& p : rows) {
			if (y > box.size.y - 8.f) break;
			NVGcolor dot = p.tx ? rcRed() : akTheme(nvgRGB(0x2a,0xa8,0x55), nvgRGB(0x3a,0xd0,0x6a));
			nvgBeginPath(vg); nvgCircle(vg, 9, y, 2.6f); nvgFillColor(vg, dot); nvgFill(vg);
			char cnt[16]; (void) std::snprintf(cnt, sizeof(cnt), "%ld", p.intervals);
			float cw = textWidth(vg, FONT_REG, 8.f, cnt);
			drawTxt(vg, FONT_REG, w - 6, y, 8.f, rcTextDim(), cnt, NVG_ALIGN_RIGHT);
			drawTxt(vg, FONT_BOLD, 16, y, 8.5f, rcText(), p.label, NVG_ALIGN_LEFT, w - 22 - cw - 4);
			y += 13.f;
		}
	}
};

// Code-drawn panel decoration: title, the RECORD label above the button, and the AK mark.
struct Decor : Widget {
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		// Header title: same font + size as the Looper's "LOOPER" (FONT_BOLD 15).
		drawTxt(vg, FONT_BOLD, box.size.x / 2, 21.f, 15.f, rcText(), "REC", NVG_ALIGN_CENTER);
		// "RECORD" label above the bezel button, whose centre sits at y=70 (aligned with
		// the Looper's OVERDUB so the two line up when placed around Ninjam).
		drawTxt(vg, FONT_BOLD, box.size.x / 2, 50.f, 11.f, rcText(), "RECORD", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, box.size.x / 2, mm2px(AK_MARK_Y_MM), 16.f, rcText(), "AK", NVG_ALIGN_CENTER);
		Widget::draw(args);
	}
};

struct RecorderWidget : ModuleWidget {
	Recorder* rec = nullptr;

	explicit RecorderWidget(Recorder* module) {
		rec = module;
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Recorder.svg"),
			asset::plugin(pluginInstance, "res/Recorder-dark.svg")));
		addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		Decor* decor = new Decor;
		decor->box.size = box.size;
		addChild(decor);

		const float W = box.size.x;
		// REC: the component-library bezel button + red light — identical to the Looper's
		// OVERDUB, at the same y so the two align. The light is driven in step().
		addParam(createLightParamCentered<VCVLightBezelLatch<RedLight>>(
			Vec(W / 2, 70.f), module, Recorder::REC_PARAM, Recorder::REC_LIGHT));

		TxToggleButton* txBtn = createParam<TxToggleButton>(Vec(W / 2 - 27, 92.f), module, Recorder::TX_PARAM);
		txBtn->box.size = Vec(54, 16);
		txBtn->rec = module;
		addParam(txBtn);

		RecStatusView* sv = new RecStatusView;
		sv->rec = module;
		sv->box.pos = Vec(4, 116.f);
		sv->box.size = Vec(W - 8, mm2px(AK_MARK_Y_MM - 4.f) - 116.f);
		addChild(sv);
	}

	// The latch params mirror Ninjam's arm/recordTx state: a user toggle pushes to the
	// link; an external change (e.g. Ninjam clearing it) pulls back into the param — the
	// same param<->state reconcile Ninjam's metronome uses. Also drives the REC light.
	void step() override {
		ModuleWidget::step();
		if (!rec) return;
		akaudio::RecorderLink* lk = rec->link();
		auto reconcile = [&](int pid, bool& prev, bool state, void (akaudio::RecorderLink::*setter)(bool)) {
			bool pOn = rec->params[pid].getValue() > 0.5f;
			if (pOn != prev) { prev = pOn; if (lk) (lk->*setter)(pOn); }
			else if (lk && state != pOn) { rec->params[pid].setValue(state ? 1.f : 0.f); prev = state; }
			else if (!lk && pOn) { rec->params[pid].setValue(0.f); prev = false; }
		};
		reconcile(Recorder::REC_PARAM, rec->prevRec, lk && lk->recArmed(), &akaudio::RecorderLink::setRecArmed);
		reconcile(Recorder::TX_PARAM, rec->prevTx, lk ? lk->recordOwnTx() : true, &akaudio::RecorderLink::setRecordOwnTx);
		if (lk && lk->sessionBase() != rec->sessionBase) lk->setSessionBase(rec->sessionBase);
		bool active = lk && lk->recActive();
		float b = active ? 1.f : ((lk && lk->recArmed()) ? 0.35f : 0.f);
		rec->lights[Recorder::REC_LIGHT].setBrightness(b);

		// Auto-export the Live set when recording stops. The jam root is cached once at
		// the arm edge (the folder name is stamped per arm and constant while recording;
		// the Ninjam module may be detached by the time recording ends). The export
		// waits a wall-clock grace so the Looper's worker can flush the disarm-boundary
		// take — NjArchive::stop() already joined its writer, so index.jsonl is
		// complete — and runs on its own thread (multi-MB build; step() must not stall).
		if (active && !rec->prevActive) {
			// Re-armed while an export was still pending: the finished jam's folder is
			// complete, so export it NOW — before lastJamRoot is repointed below.
			if (rec->exportAt >= 0.0 && !rec->lastJamRoot.empty())
				launchAutoExport(rec->lastJamRoot, rec->liteExport);
			rec->exportAt = -1.0;
			rec->lastJamRoot = akaudio::expandHome(lk->sessionBase()) + "/" + lk->recSessionName();
		}
		if (rec->prevActive && !active && rec->autoExportAls && !rec->lastJamRoot.empty())
			rec->exportAt = system::getTime() + 1.5;
		rec->prevActive = active;
		if (rec->exportAt >= 0.0 && system::getTime() >= rec->exportAt) {
			rec->exportAt = -1.0;
			launchAutoExport(rec->lastJamRoot, rec->liteExport);
		}
	}

	void appendContextMenu(Menu* menu) override {
		Recorder* m = getModule<Recorder>();
		if (!m) return;
		const akaudio::RecorderLink* lk = m->link();
		menu->addChild(new MenuSeparator);
		// Export works without Ninjam adjacent (it only reads the jam folder on disk) —
		// a player can export an old jam without re-racking. Recording still needs Ninjam.
		menu->addChild(createMenuItem("Export Ableton Live set (.als)\xe2\x80\xa6", "", [m]() {
			std::string def = !m->lastJamRoot.empty() ? m->lastJamRoot : m->sessionBase;
			char* pick = osdialog_file(OSDIALOG_OPEN_DIR, def.c_str(), NULL, NULL);
			if (!pick) return;
			std::string root = pick;
			std::free(pick);
			std::string whyNot;
			std::string out = akaudio::exportJamAls(root, m->liteExport, &whyNot);
			// No Finder/Explorer popup — a dialog names the file; the "Open jams
			// folder" menu item exists for whoever wants the window.
			if (!out.empty())
				osdialog_message(OSDIALOG_INFO, OSDIALOG_OK,
					("Exported:\n" + akaudio::collapseHome(out)).c_str());
			else
				osdialog_message(OSDIALOG_INFO, OSDIALOG_OK, whyNot.c_str());
		}));
		menu->addChild(createBoolPtrMenuItem("Export .als when recording stops", "", &m->autoExportAls));
		// Target Live edition: Lite's 8-track cap gets the reduced flavor — 6 grid
		// tracks, all players merged onto one lane (overlaps audio-mixed), TX on the
		// 8th. Standard/Suite gets the full set (8 grid tracks + a track per player).
		menu->addChild(createIndexSubmenuItem("Target Live edition",
			{"Standard / Suite (full)", "Lite (8 tracks: 6 loops + players + TX)"},
			[m]() { return m->liteExport ? 1 : 0; },
			[m](int i) { m->liteExport = i == 1; }));
		menu->addChild(new MenuSeparator);
		if (!lk) {
			menu->addChild(createMenuLabel("Place directly next to a Ninjam module"));
			return;
		}
		menu->addChild(createMenuLabel("Saves raw NINJAM intervals to disk (no re-encode)"));
		if (lk->recActive())
			menu->addChild(createMenuLabel("Recording: " + lk->recSessionName()));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Jams folder: " + m->sessionBase));
		menu->addChild(createMenuItem("Choose folder\xe2\x80\xa6", "", [m]() {
			char* path = osdialog_file(OSDIALOG_OPEN_DIR, m->sessionBase.c_str(), NULL, NULL);
			if (path) { m->sessionBase = path; std::free(path); }
		}));
		menu->addChild(createMenuItem("Reset to ~/Music/jams", "", [m]() {
			m->sessionBase = akaudio::defaultJamsDir();
		}, m->sessionBase == akaudio::defaultJamsDir()));
		menu->addChild(createMenuItem("Open jams folder", "", [m]() {
			system::openDirectory(m->sessionBase);
		}));
	}
};

Model* modelRecorder = createModel<Recorder, RecorderWidget>("Recorder");
