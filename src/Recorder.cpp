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

#include <atomic>

namespace {

inline NVGcolor rcText()    { return akTheme(nvgRGB(0x24, 0x27, 0x2b), nvgRGB(0xed, 0xed, 0xed)); }
inline NVGcolor rcTextDim() { return akTheme(nvgRGB(0x5c, 0x61, 0x68), nvgRGB(0x9a, 0xa0, 0xa6)); }
inline NVGcolor rcRed()     { return akTheme(nvgRGB(0xc0, 0x39, 0x2b), nvgRGB(0xe0, 0x60, 0x52)); }
inline NVGcolor rcWell()    { return akShade(akDark() ? 0x14 : 0x10); }
inline NVGcolor rcCard()    { return akTheme(nvgRGB(0xd6, 0xd9, 0xdc), nvgRGB(0x2e, 0x31, 0x34)); }
inline NVGcolor rcBorder()  { return akShade(akDark() ? 0x2e : 0x26); }

} // namespace

struct Recorder : Module {
	enum ParamId { REC_PARAM, TX_PARAM, PARAMS_LEN };
	enum InputId { INPUTS_LEN };
	enum OutputId { OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	dsp::BooleanTrigger recTrig, txTrig;

	Recorder() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configButton(REC_PARAM, "Record (arm the wire archive)");
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
};

// ------------------------------------------------------------------------------------

struct RecArmButton : app::Switch {
	Recorder* rec = nullptr;
	bool hovered = false;
	RecArmButton() { momentary = true; }
	void onEnter(const EnterEvent& e) override { hovered = true; app::Switch::onEnter(e); }
	void onLeave(const LeaveEvent& e) override { hovered = false; app::Switch::onLeave(e); }
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		akaudio::RecorderLink* lk = rec ? rec->link() : nullptr;
		bool armed = lk && lk->recArmed();
		bool active = lk && lk->recActive();
		const float cx = box.size.x / 2, cy = box.size.y / 2, r = std::min(cx, cy) - 2.f;
		// Filled red disc = armed; a soft pulse when actually writing.
		if (armed && active) {
			nvgBeginPath(vg);
			nvgCircle(vg, cx, cy, r + 3.f);
			float a = 0.25f + 0.2f * (float) (0.5 + 0.5 * std::sin(system::getTime() * 2.0 * M_PI));
			nvgFillColor(vg, nvgTransRGBAf(rcRed(), a));
			nvgFill(vg);
		}
		nvgBeginPath(vg);
		nvgCircle(vg, cx, cy, r);
		nvgFillColor(vg, armed ? rcRed() : (hovered ? akShade(0x22) : rcWell()));
		nvgFill(vg);
		nvgStrokeColor(vg, rcBorder());
		nvgStrokeWidth(vg, 1.f);
		nvgStroke(vg);
	}
};

struct TxToggleButton : app::Switch {
	Recorder* rec = nullptr;
	bool hovered = false;
	void onEnter(const EnterEvent& e) override { hovered = true; app::Switch::onEnter(e); }
	void onLeave(const LeaveEvent& e) override { hovered = false; app::Switch::onLeave(e); }
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		akaudio::RecorderLink* lk = rec ? rec->link() : nullptr;
		bool on = lk && lk->recordOwnTx();
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, 3.f);
		nvgFillColor(vg, on ? nvgTransRGBAf(rcRed(), 0.3f) : rcCard());
		nvgFill(vg);
		nvgStrokeColor(vg, rcBorder());
		nvgStroke(vg);
		drawTxt(vg, FONT_BOLD, box.size.x / 2, box.size.y / 2, 8.f,
			on ? rcText() : rcTextDim(), "+TX", NVG_ALIGN_CENTER);
	}
};

// The whole body below the header: status line + per-player list.
struct RecStatusView : Widget {
	Recorder* rec = nullptr;
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		const float w = box.size.x;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, w, box.size.y, 4.f);
		nvgFillColor(vg, rcWell());
		nvgFill(vg);
		nvgStrokeColor(vg, rcBorder());
		nvgStroke(vg);

		akaudio::RecorderLink* lk = rec ? rec->link() : nullptr;
		float y = 14.f;
		if (!lk) {
			drawTxt(vg, FONT_BOLD, 8, y, 9.f, rcTextDim(), "Place next to Ninjam", NVG_ALIGN_LEFT, w - 16);
			return;
		}
		std::string head;
		if (lk->recActive()) head = "\xe2\x97\x8f REC \xc2\xb7 " + lk->recSessionName();
		else if (lk->recArmed() && !lk->recJoined()) head = "armed \xc2\xb7 waiting to join";
		else if (lk->recArmed()) head = "armed";
		else head = "idle";
		drawTxt(vg, FONT_BOLD, 8, y, 9.f, lk->recActive() ? rcRed() : rcTextDim(), head, NVG_ALIGN_LEFT, w - 16);
		y += 15.f;
		char n[32];
		std::snprintf(n, sizeof(n), "%ld intervals", lk->recIntervals());
		drawTxt(vg, FONT_REG, 8, y, 8.5f, rcTextDim(), n, NVG_ALIGN_LEFT, w - 16);
		y += 16.f;
		nvgBeginPath(vg);
		nvgMoveTo(vg, 8, y - 6.f); nvgLineTo(vg, w - 8, y - 6.f);
		nvgStrokeColor(vg, rcBorder()); nvgStroke(vg);
		for (const auto& p : lk->recStatus()) {
			if (y > box.size.y - 8.f) break;
			drawTxt(vg, FONT_REG, 8, y, 8.5f, p.tx ? rcRed() : rcText(), p.label, NVG_ALIGN_LEFT, w - 70);
			char c[48];
			double mb = p.bytes / (1024.0 * 1024.0);
			std::snprintf(c, sizeof(c), "%ld \xc2\xb7 %.1f MB", p.intervals, mb);
			drawTxt(vg, FONT_REG, w - 8, y, 8.f, rcTextDim(), c, NVG_ALIGN_RIGHT);
			y += 14.f;
		}
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

		const float W = box.size.x;

		RecArmButton* recBtn = createParam<RecArmButton>(Vec(W / 2 - 13, 26), module, Recorder::REC_PARAM);
		recBtn->box.size = Vec(26, 26);
		recBtn->rec = module;
		addParam(recBtn);

		TxToggleButton* txBtn = createParam<TxToggleButton>(Vec(W / 2 - 15, 58), module, Recorder::TX_PARAM);
		txBtn->box.size = Vec(30, 14);
		txBtn->rec = module;
		addParam(txBtn);

		RecStatusView* sv = new RecStatusView;
		sv->rec = module;
		sv->box.pos = Vec(6, 80);
		sv->box.size = Vec(W - 12, RACK_GRID_HEIGHT - 80 - 22);
		addChild(sv);
	}

	// REC / +TX buttons act on the RecorderLink (thread ops) — handle their edges here,
	// on the UI thread. The params stay two-state so they are MIDI-mappable.
	void step() override {
		ModuleWidget::step();
		if (!rec) return;
		akaudio::RecorderLink* lk = rec->link();
		if (rec->recTrig.process(rec->params[Recorder::REC_PARAM].getValue() > 0.5f) && lk)
			lk->setRecArmed(!lk->recArmed());
		if (rec->txTrig.process(rec->params[Recorder::TX_PARAM].getValue() > 0.5f) && lk)
			lk->setRecordOwnTx(!lk->recordOwnTx());
	}

	void appendContextMenu(Menu* menu) override {
		Recorder* m = getModule<Recorder>();
		if (!m) return;
		akaudio::RecorderLink* lk = m->link();
		menu->addChild(new MenuSeparator);
		if (!lk) {
			menu->addChild(createMenuLabel("Place directly next to a Ninjam module"));
			return;
		}
		menu->addChild(createMenuLabel("Wire archive: raw NINJAM intervals to disk"));
		if (lk->recActive())
			menu->addChild(createMenuLabel("Session: " + lk->recSessionName()));
	}
};

Model* modelRecorder = createModel<Recorder, RecorderWidget>("Recorder");
