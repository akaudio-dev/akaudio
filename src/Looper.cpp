// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

// Looper — 8 tracks × 8 slots Ableton-Session-style interval looper for NINJAM jams.
// Design: docs/LOOPER_DESIGN.md. This file is the Rack glue: the Module (params,
// jacks, clock source, persistence) and the panel widgets. The looper itself —
// always-record, boundary-quantized capture/launch/stop/overdub, scenes, repeats and
// decay, the submix — is the Rack-free LooperEngine in src/looper/ (M1), driven by an
// adjacent Ninjam's JamClockMessage (M0) or, without one, a simulated clock. Committed
// takes are encoded to OGG + indexed on disk by looper/Session (M4). Panel polish: M5.

#include "plugin.hpp"
#include "Theme.hpp"
#include "JamClock.hpp"
#include "RecorderLink.hpp"
#include "looper/LooperEngine.hpp"
#include "looper/Session.hpp"
#include "looper/SessionMirror.hpp"

#include <osdialog.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iterator>

namespace {

const int TRACKS = akaudio::looper::MAX_TRACKS;
const int SLOTS = akaudio::looper::MAX_SLOTS;
const int THUMB_BINS = akaudio::looper::THUMB_BINS;

const int REPEAT_CHOICES[] = {0, 1, 2, 4, 8, 16, 32, 64}; // 0 = ∞ (cell-menu choices)
const int N_REPEAT_CHOICES = 8;
const float DECAY_CHOICES[] = {0.f, -1.f, -2.f, -3.f, -6.f}; // dB per repetition (menus)
const int N_DECAY_CHOICES = 5;
// Shared menu labels (cell menu + module "New clips" defaults — keep them in sync).
static const std::vector<std::string>& repeatLabels() {
	static const std::vector<std::string> v = {"\xe2\x88\x9e", "1", "2", "4", "8", "16", "32", "64"};
	return v;
}
static const std::vector<std::string>& decayLabels() {
	static const std::vector<std::string> v = {"0 dB (none)", "−1 dB", "−2 dB", "−3 dB", "−6 dB"};
	return v;
}
const int SIM_SECONDS[] = {2, 4, 8, 16, 32};
const int N_SIM_SECONDS = 5;

inline NVGcolor lpText()    { return akTheme(nvgRGB(0x24, 0x27, 0x2b), nvgRGB(0xed, 0xed, 0xed)); }
inline NVGcolor lpTextDim() { return akTheme(nvgRGB(0x5c, 0x61, 0x68), nvgRGB(0x9a, 0xa0, 0xa6)); }
inline NVGcolor lpGreen()   { return akTheme(nvgRGB(0x2a, 0xa8, 0x55), nvgRGB(0x3a, 0xd0, 0x6a)); }
inline NVGcolor lpAmber()   { return akTheme(nvgRGB(0xd9, 0x8b, 0x1a), nvgRGB(0xf0, 0xb0, 0x40)); }
inline NVGcolor lpRed()     { return akTheme(nvgRGB(0xc0, 0x39, 0x2b), nvgRGB(0xe0, 0x60, 0x52)); }
inline NVGcolor lpWell()    { return akShade(akDark() ? 0x14 : 0x10); }
inline NVGcolor lpCard()    { return akTheme(nvgRGB(0xd6, 0xd9, 0xdc), nvgRGB(0x2e, 0x31, 0x34)); }
inline NVGcolor lpBorder()  { return akShade(akDark() ? 0x2e : 0x26); }
inline NVGcolor lpRing()    { return akTheme(nvgRGB(0x1f, 0x1f, 0x1f), nvgRGB(0xff, 0xff, 0xff)); }

// ---- Layout (px; 44 HP = 660 × 380) ----
// Grid on the left, scene column beside it, then a controls column (DUB, the output
// plates) to the right. The output plates / input well reuse the
// Theme.hpp plate geometry (AK_PLATE_*) so they read as the same family as Radio and
// Ninjam; the "AK" mark sits at the shared AK_MARK_Y_MM.
const float COL_X0 = 8.f, COL_W = 68.f;        // track columns: L+R jack pair reads as a pair
const float SCENE_X = 558.f, SCENE_W = 34.f;   // scene column
const float JACK_Y = 54.f, JACK_DX = 13.5f;    // L/R jack centers at colCx ± JACK_DX
const float NAME_Y = 70.f, NAME_H = 14.f;      // editable track label (MindMeld-style)
const float GRID_Y = 92.f, ROW_H = 29.f, BTN_H = 26.f, BTN_W = 64.f;
const float STOP_GAP = 6.f;                    // whitespace between the grid and the stop row (= grid↔scenes gap)
const float STOP_Y = GRID_Y + 7 * ROW_H + BTN_H + STOP_GAP; // bottoms align with the MIX plate (stopH())
// Controls column (x center RX), top → bottom.
const float RX = 623.f;
const float Y_DUB = 96.f;                      // overdub bezel button (label above, like Fundamental's PUSH),
                                               // centered in the column gap above the OUTS plate
// Three stacked output plates: OUTS (the per-track poly out) over CUE (private) over MIX
// (to Ninjam). CUE/MIX are vertical stereo plates (L over R, jacks squeezed together but
// with clear margins — room above for the title, beside for the L/R labels); OUTS is a
// single poly jack (MindMeld-style gold ring). Spread to fill the column; the MIX plate
// bottom aligns with Ninjam's lower output plate bottom, so the two modules line up.
const float PLATE_W = 50.f;                    // plate width (jack + L/R label to its left, with margins)
const float PLATE_JDY = 28.f;                  // L→R jack spacing (close, but not touching)
const float PLATE_TOPGAP = 27.f;               // plate top → L jack: room for the title, clear of the jack
const float PLATE_BOTGAP = 14.f;               // R jack → plate bottom (jack has margin below)
const float PLATE_H = PLATE_TOPGAP + PLATE_JDY + PLATE_BOTGAP;
const float PLATE_JACK_X = RX + 7.f, PLATE_LAB_X = RX - 15.f;
const float PLATE_TITLE_DY = 7.f;              // title center below plate top (sits above the jack)
const float OUTS_TOPGAP = 28.f;                // OUTS plate top → jack: room for title + the poly ring
const float OUTS_BOTGAP = 16.f;
const float OUTS_H = OUTS_TOPGAP + OUTS_BOTGAP;
const float PLATE_GAP = 8.f;                   // gap between stacked plates
// MIX bottom = Ninjam's lower (CV) output plate bottom → the modules line up; stack up.
const float MIX_BOT = mm2px(AK_PLATE_TOP_MM + AK_PLATE_H_MM);
const float MIX_TOP = MIX_BOT - PLATE_H;
const float CUE_TOP = MIX_TOP - PLATE_GAP - PLATE_H;
const float OUTS_TOP = CUE_TOP - PLATE_GAP - OUTS_H;
const float OUTS_JACK_Y = OUTS_TOP + OUTS_TOPGAP;
inline float plateLY(float top) { return top + PLATE_TOPGAP; }
inline float plateRY(float top) { return top + PLATE_TOPGAP + PLATE_JDY; }
inline float stopH() { return MIX_BOT - STOP_Y; } // stops grow down to Ninjam's output-plate bottom

// Track label: MindMeld's amber-on-black on the dark panel; black on grey on the light one.
inline NVGcolor lpLabelBg()   { return akTheme(nvgRGB(0xc4, 0xc7, 0xcb), nvgRGB(0x1a, 0x1a, 0x1a)); }
inline NVGcolor lpLabelText() { return akTheme(nvgRGB(0x1f, 0x1f, 0x1f), nvgRGB(0xf2, 0xc0, 0x3a)); }
// Raised button body (scene / stop / stop-all): pill-ish, solid, no border — distinct
// from the sunken, bordered clip cells.
inline NVGcolor lpButton()    { return akTheme(nvgRGB(0xb9, 0xbd, 0xc2), nvgRGB(0x44, 0x48, 0x4c)); }
inline NVGcolor lpButtonHi()  { return akTheme(nvgRGB(0xa8, 0xac, 0xb2), nvgRGB(0x52, 0x56, 0x5b)); }

inline float colX(int t) { return COL_X0 + t * COL_W; }
inline float colCx(int t) { return colX(t) + COL_W / 2; }

int repeatsIndex(int reps) {
	for (int i = 0; i < N_REPEAT_CHOICES; i++)
		if (REPEAT_CHOICES[i] == reps) return i;
	return 0;
}
int decayIndex(float db) {
	int best = 0;
	for (int i = 1; i < N_DECAY_CHOICES; i++)
		if (std::fabs(DECAY_CHOICES[i] - db) < std::fabs(DECAY_CHOICES[best] - db)) best = i;
	return best;
}

} // namespace

struct Looper : Module {
	enum ParamId {
		ENUMS(SLOT_PARAM, TRACKS * SLOTS), // index = track * SLOTS + slot
		ENUMS(SCENE_PARAM, SLOTS),
		ENUMS(STOP_PARAM, TRACKS),
		STOP_ALL_PARAM,
		ENUMS(TX_PARAM, TRACKS),            // latch: this track's live input goes to MIX (on air)
		OVERDUB_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		ENUMS(IN_L_INPUT, TRACKS),
		ENUMS(IN_R_INPUT, TRACKS),
		MULTI_INPUT,   // poly: channels 2t, 2t+1 = track t L/R (a track's own L jack wins)
		INPUTS_LEN
	};
	enum OutputId {
		MIX_L_OUTPUT, MIX_R_OUTPUT,
		CUE_L_OUTPUT, CUE_R_OUTPUT,
		POLY_OUTPUT,   // per-track direct out (poly): channels 2t, 2t+1 = track t L/R
		OUTPUTS_LEN
	};
	enum LightId { OVERDUB_LIGHT, LIGHTS_LEN };

	// The looper proper (Rack-free, src/looper/): tracks × slots, rolling record,
	// boundary commits, playback, submix. Its worker thread owns all buffers.
	akaudio::looper::LooperEngine engine;

	// M4: the disk side. The engine's worker hands committed takes here to be encoded to
	// OGG + indexed under `<sessionBase>/<stamp>_<room>/looper/` (shared with a Recorder's
	// jam folder when one is armed). UI thread points it at the right folder (syncSession).
	akaudio::looper::Session session;
	std::string sessionBase = akaudio::defaultJamsDir(); // where jam folders go (persisted, templated)
	std::string ownSessionFolder;      // <stamp>_session when jamming without a Recorder (formed once)
	std::string resolvedSessionDir;    // the looper/ dir last handed to the session
	std::string appliedSessionBase;    // sessionBase at the last resolve (menu change ⇒ re-resolve)
	std::string loadDir;               // session dir restored from the patch (clip loader)
	bool loadPending = false;          // a restore is queued (run once on the UI thread)
	// Embed the live grid in the patch (Rack patch storage): onSave mirrors the session
	// folder's live cells into the .vcv; a load whose session folder is gone hydrates a
	// fresh one from that snapshot. The session folder stays the source of truth.
	bool embedTakes = true;
	bool sessionRestored = false;      // this session was restored — keep its folder, don't reform

	// Editable track labels (UI thread only; persisted). Default "-01-" … "-08-".
	std::string trackNames[TRACKS];
	// Follow a connected MindMeld MixMaster's track labels (menu toggle): while on, the
	// widget's sweep copies the mixer's names over ours (local renames are overwritten —
	// that is what "follow" means). Off by default.
	bool followMixerNames = false;

	// Selection = the last pressed slot (the continuous overdub's target). Index
	// track*SLOTS+slot, −1 none.
	std::atomic<int> selected{-1};

	// ---- Clock source ----
	// A live Ninjam clock (expander message, either side, chained) wins; otherwise the
	// simulated clock runs so the looper works with nothing else in the rack.
	std::atomic<int> simSecondsIdx{1};      // index into SIM_SECONDS (4 s)
	std::atomic<bool> ninjamClock{false};   // UI: source LED / header
	std::atomic<int> clockBpm{0}, clockBpi{0};
	// Where each track's signal comes from (UI tag): 0 none, 1 own jack, 2 MULTI pair.
	std::atomic<int> trackSource[TRACKS];
	std::atomic<int> trackMultiCh[TRACKS]; // first MULTI channel (0-based) feeding this track, −1 none
	std::atomic<float> trackLevel[TRACKS]; // live input peak (UI: is signal actually arriving?)
	std::atomic<int> multiChannels{0};     // MULTI poly channel count (UI diagnostic)
	std::atomic<float> phase{0.f};          // 0..1 interval progress (UI)
	std::atomic<float> secsLeft{0.f};       // seconds to the next boundary (UI)
	int64_t simFrame = -1;                  // simulated clock position (−1 = first frame)
	uint64_t simSession = 0;                // simulated clock timeline
	uint32_t simGen = 1000;                 // simulated clock generation (bumped on source switch)
	uint64_t lastSession[2] = {0, 0};       // liveness: Ninjam advances sessionFrame every frame
	bool lastNinjam = false;

	dsp::BooleanTrigger slotTrig[TRACKS * SLOTS];
	dsp::BooleanTrigger sceneTrig[SLOTS];
	dsp::BooleanTrigger stopTrig[TRACKS];
	dsp::BooleanTrigger stopAllTrig;

	Looper() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		for (int t = 0; t < TRACKS; t++) {
			trackNames[t] = defaultTrackName(t);
			trackSource[t].store(0, std::memory_order_relaxed);
			trackMultiCh[t].store(-1, std::memory_order_relaxed);
			trackLevel[t].store(0.f, std::memory_order_relaxed);
		}
		// Expander message buffers (both sides — Ninjam may be left or right). Ninjam
		// writes the producer side, Rack flips at the end of the timestep, we read the
		// consumer side. Freed in the destructor.
		leftExpander.producerMessage = new akaudio::JamClockMessage();
		leftExpander.consumerMessage = new akaudio::JamClockMessage();
		rightExpander.producerMessage = new akaudio::JamClockMessage();
		rightExpander.consumerMessage = new akaudio::JamClockMessage();
		for (int t = 0; t < TRACKS; t++) {
			for (int s = 0; s < SLOTS; s++)
				configButton(SLOT_PARAM + t * SLOTS + s, string::f("Track %d slot %d", t + 1, s + 1));
			configButton(STOP_PARAM + t, string::f("Stop track %d", t + 1));
			configSwitch(TX_PARAM + t, 0.f, 1.f, 1.f, string::f("Track %d transmit (live input to MIX)", t + 1),
				{"Private", "On air"});
			configInput(IN_L_INPUT + t, string::f("Track %d L", t + 1));
			configInput(IN_R_INPUT + t, string::f("Track %d R (unpatched = mono)", t + 1));
		}
		configInput(MULTI_INPUT, "Multi (poly): channels 1-2 = track 1, 3-4 = track 2, â¦; a track's own jack wins its pair");
		for (int s = 0; s < SLOTS; s++)
			configButton(SCENE_PARAM + s, string::f("Scene %d", s + 1));
		configButton(STOP_ALL_PARAM, "Stop all tracks");
		configSwitch(OVERDUB_PARAM, 0.f, 1.f, 0.f, "Overdub mode", {"Off", "On"});
		configOutput(MIX_L_OUTPUT, "Mix L (to Ninjam IN)");
		configOutput(MIX_R_OUTPUT, "Mix R (to Ninjam IN)");
		configOutput(CUE_L_OUTPUT, "Cue L (private / non-transmitting tracks â your monitor)");
		configOutput(CUE_R_OUTPUT, "Cue R (private / non-transmitting tracks â your monitor)");
		configOutput(POLY_OUTPUT, "Per-track direct out (poly: ch 1-2 = track 1, 3-4 = track 2, ...)");
		engine.setSink(&session); // takes are encoded + indexed by the session (M4)
		engine.start();
	}

	~Looper() override {
		engine.stop();
		delete (akaudio::JamClockMessage*) leftExpander.producerMessage;
		delete (akaudio::JamClockMessage*) leftExpander.consumerMessage;
		delete (akaudio::JamClockMessage*) rightExpander.producerMessage;
		delete (akaudio::JamClockMessage*) rightExpander.consumerMessage;
	}

	static std::string defaultTrackName(int t) { return string::f("-%02d-", t + 1); }

	// ---- Button intents (audio thread) ----
	void pressSlot(int t, int s) {
		selected.store(t * SLOTS + s, std::memory_order_relaxed);
		engine.pressSlot(t, s, params[OVERDUB_PARAM].getValue() > 0.5f);
	}

	// Pick the clock for this frame: a live Ninjam message from either side, else the
	// simulated clock. Fills the engine's ClockFrame.
	akaudio::looper::ClockFrame tickClock(const ProcessArgs& args) {
		const akaudio::JamClockMessage* src = nullptr;
		for (int side = 0; side < 2; side++) {
			const akaudio::JamClockMessage* m = (const akaudio::JamClockMessage*)
				(side ? rightExpander.consumerMessage : leftExpander.consumerMessage);
			if (!m) continue;
			// Live = running and advancing: a removed Ninjam leaves a frozen message.
			bool live = m->running && m->sessionFrame != lastSession[side];
			lastSession[side] = m->sessionFrame;
			if (live && !src) src = m;
		}
		akaudio::looper::ClockFrame c;
		c.running = true;
		c.sampleRate = args.sampleRate;
		if (src) {
			c.intervalFrames = std::max(1, src->intervalFrames);
			c.frameInInterval = src->frameInInterval;
			c.downbeat = src->downbeat;
			c.gridGeneration = src->gridGeneration;
			c.sessionFrame = src->sessionFrame;
			c.bpm = src->bpm; c.bpi = src->bpi;
			simFrame = -1;
		} else {
			if (lastNinjam) simGen++; // back to the simulated grid: a new generation
			int64_t n = std::max<int64_t>(1,
				(int64_t) (SIM_SECONDS[simSecondsIdx.load(std::memory_order_relaxed)] * args.sampleRate));
			if (++simFrame >= n) simFrame = 0;
			c.intervalFrames = (int) n;
			c.frameInInterval = (int) simFrame;
			c.downbeat = simFrame == 0;
			c.gridGeneration = simGen;
			c.sessionFrame = simSession++;
			c.bpm = 0; c.bpi = 0;
		}
		lastNinjam = src != nullptr;
		ninjamClock.store(lastNinjam, std::memory_order_relaxed);
		clockBpm.store(c.bpm, std::memory_order_relaxed);
		clockBpi.store(c.bpi, std::memory_order_relaxed);
		phase.store((float) c.frameInInterval / (float) c.intervalFrames, std::memory_order_relaxed);
		secsLeft.store((float) (c.intervalFrames - c.frameInInterval) / args.sampleRate, std::memory_order_relaxed);
		return c;
	}

	void process(const ProcessArgs& args) override {
		// Buttons (edges on the audio thread, where the engine's state machine is).
		for (int i = 0; i < TRACKS * SLOTS; i++)
			if (slotTrig[i].process(params[SLOT_PARAM + i].getValue() > 0.5f))
				pressSlot(i / SLOTS, i % SLOTS);
		for (int s = 0; s < SLOTS; s++)
			if (sceneTrig[s].process(params[SCENE_PARAM + s].getValue() > 0.5f))
				engine.pressScene(s);
		for (int t = 0; t < TRACKS; t++)
			if (stopTrig[t].process(params[STOP_PARAM + t].getValue() > 0.5f))
				engine.stopTrack(t);
		if (stopAllTrig.process(params[STOP_ALL_PARAM].getValue() > 0.5f))
			engine.stopAll();

		// Inputs: a track's own jack pair, else its MULTI pair (sequential stereo pairs).
		akaudio::looper::TrackIn in[TRACKS];
		const int nMulti = inputs[MULTI_INPUT].getChannels(); // 0 when unpatched
		multiChannels.store(nMulti, std::memory_order_relaxed);
		for (int t = 0; t < TRACKS; t++) {
			akaudio::looper::TrackIn& x = in[t];
			x.tx = params[TX_PARAM + t].getValue() > 0.5f;
			int src = 0, mch = -1;
			if (inputs[IN_L_INPUT + t].isConnected()) {
				x.present = true;
				x.l = inputs[IN_L_INPUT + t].getVoltage() * 0.2f;
				x.r = inputs[IN_R_INPUT + t].isConnected() ? inputs[IN_R_INPUT + t].getVoltage() * 0.2f : x.l;
				src = 1;
			} else if (2 * t < nMulti) {
				mch = 2 * t;
				x.present = true;
				x.l = inputs[MULTI_INPUT].getVoltage(2 * t) * 0.2f;
				x.r = (2 * t + 1 < nMulti) ? inputs[MULTI_INPUT].getVoltage(2 * t + 1) * 0.2f : x.l;
				src = 2;
			} else {
				x.present = false;
				x.l = x.r = 0.f;
			}
			trackSource[t].store(src, std::memory_order_relaxed);
			trackMultiCh[t].store(mch, std::memory_order_relaxed);
			float lvl = std::max(std::fabs(x.present ? x.l : 0.f), std::fabs(x.present ? x.r : 0.f));
			float prev = trackLevel[t].load(std::memory_order_relaxed);
			trackLevel[t].store(lvl > prev ? lvl : prev * 0.999f, std::memory_order_relaxed); // peak-hold
		}

		float outL = 0.f, outR = 0.f, cueL = 0.f, cueR = 0.f;
		float trackLR[TRACKS * 2] = {};
		// Route the MIX to your rig (and on to Ninjam's IN) with real cables through the
		// mixer — there is no invisible Looper→Ninjam link. So the submix is only worth
		// computing when the MIX jacks are actually patched.
		const bool wantMix = outputs[MIX_L_OUTPUT].isConnected() || outputs[MIX_R_OUTPUT].isConnected();
		// Continuous overdub: hand the engine the OVERDUB latch + current selection so the
		// selected playing cell overdubs each interval while engaged (§ boundary re-target).
		engine.overdubMode.store(params[OVERDUB_PARAM].getValue() > 0.5f, std::memory_order_relaxed);
		engine.overdubSel.store(selected.load(std::memory_order_relaxed), std::memory_order_relaxed);
		engine.tick(tickClock(args), in, TRACKS, system::getTime(), outL, outR, cueL, cueR, trackLR, wantMix);
		outputs[MIX_L_OUTPUT].setVoltage(outL * 5.f);
		outputs[MIX_R_OUTPUT].setVoltage(outR * 5.f);
		outputs[CUE_L_OUTPUT].setVoltage(cueL * 5.f);
		outputs[CUE_R_OUTPUT].setVoltage(cueR * 5.f);
		// POLY direct out: track t on channels 2t (L) / 2t+1 (R) — mirrors the MULTI input.
		outputs[POLY_OUTPUT].setChannels(TRACKS * 2);
		for (int t = 0; t < TRACKS; t++) {
			size_t c = (size_t) t * 2;
			outputs[POLY_OUTPUT].setVoltage(trackLR[c] * 5.f, t * 2);
			outputs[POLY_OUTPUT].setVoltage(trackLR[c + 1] * 5.f, t * 2 + 1);
		}
		lights[OVERDUB_LIGHT].setBrightness(params[OVERDUB_PARAM].getValue() > 0.5f ? 1.f : 0.f);
	}

	// ---- UI-thread helpers ----
	void clearSlot(int t, int s) {
		engine.requestClear(t, s);
		// Drop the cell's manifest settings right away too (the engine's reset of the
		// slot atomics is async, and a rest cell has no file — no Session::clear runs
		// for it): otherwise the row would linger until the next sweep.
		session.setSlotSettings(t, s, 0, 0.f, 0);
	}

	// UI thread (cell-menu setters + the widget sweep): reflect a slot's
	// repeats/decay/follow into the session manifest — including take-less "rest"
	// cells, which get settings-only rows (no-op when nothing changed).
	void pushSlotSettings(int t, int s) {
		akaudio::looper::Slot& sl = engine.tracks[t].slots[s];
		session.setSlotSettings(t, s,
			sl.repeats.load(std::memory_order_relaxed),
			sl.decayDb.load(std::memory_order_relaxed),
			sl.followSlot.load(std::memory_order_relaxed));
	}
	int pendingCount() const { return engine.pendingCount(); }

	// An adjacent Ninjam, as a RecorderLink — used only to borrow the exact jam folder it
	// archives to (so the looper's takes land beside the Recorder's players/ + tx/).
	akaudio::RecorderLink* adjacentRecorderLink() {
		Module* l = leftExpander.module;
		if (l && l->model == modelNinjam) return dynamic_cast<akaudio::RecorderLink*>(l);
		Module* r = rightExpander.module;
		if (r && r->model == modelNinjam) return dynamic_cast<akaudio::RecorderLink*>(r);
		return nullptr;
	}

	// Point the session at the right `.../looper` folder and keep its manifest metadata
	// current (UI thread, from the widget's step()). We prefer sharing the exact folder a
	// Recorder is archiving to; otherwise our own `<stamp>_session`. The folder is frozen
	// once a take has been written, so arming a Recorder mid-jam never splits takes across
	// two folders — but a menu change of the base folder re-resolves.
	void syncSession() {
		if (loadPending) loadSession();
		// A restored session keeps its own folder (so continued captures land beside the
		// loaded takes and their manifest) — until the user changes the base folder.
		if (sessionRestored) {
			if (sessionBase != appliedSessionBase) sessionRestored = false; // menu change ⇒ re-resolve
			else {
				for (int t = 0; t < TRACKS; t++) session.setTrackName(t, trackNames[t]);
				return;
			}
		}
		std::string base, folder;
		const akaudio::RecorderLink* rl = adjacentRecorderLink();
		if (rl && !rl->recSessionName().empty()) {
			base = akaudio::expandHome(rl->sessionBase());
			folder = rl->recSessionName();
		} else {
			base = akaudio::expandHome(sessionBase);
			if (ownSessionFolder.empty())
				ownSessionFolder = akaudio::timeStamp("%Y-%m-%d_%H%M") + "_session";
			folder = ownSessionFolder;
		}
		std::string dir = base + "/" + folder + "/looper";
		bool baseChanged = sessionBase != appliedSessionBase;
		if (resolvedSessionDir.empty() || !session.hasWritten() || baseChanged) {
			if (dir != resolvedSessionDir) {
				resolvedSessionDir = dir;
				session.setDir(dir);
				session.setRoom(folder);
			}
			appliedSessionBase = sessionBase;
		}
		for (int t = 0; t < TRACKS; t++)
			session.setTrackName(t, trackNames[t]);
	}

	// Restore the grid from a persisted session folder (clip loader). UI thread, runs once
	// on patch load: parse `<loadDir>/session.json`, restore track names, seed the session's
	// manifest model, and queue each take's OGG for the worker to decode + install. The
	// takes appear as FILLED slots (playable once the live grid matches their length).
	void loadSession() {
		loadPending = false;
		// Disk first: the saved session folder is the source of truth. Only when it is
		// gone (a shared patch, a deleted jams dir) hydrate a fresh folder from the
		// patch's embedded snapshot — patch storage is transport, never a write target.
		if (loadDir.empty() || !system::isFile(loadDir + "/session.json"))
			hydrateFromSnapshot();
		if (loadDir.empty()) return;
		std::ifstream f(loadDir + "/session.json", std::ios::binary);
		if (!f) return;
		std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		if (s.empty()) return;
		json_error_t err;
		json_t* root = json_loads(s.c_str(), 0, &err);
		if (!root) return;

		json_t* trks = json_object_get(root, "tracks");
		if (json_is_array(trks)) {
			size_t i; json_t* v;
			json_array_foreach(trks, i, v) {
				int idx = (int) json_integer_value(json_object_get(v, "index"));
				const char* nm = json_string_value(json_object_get(v, "name"));
				if (idx >= 0 && idx < TRACKS && nm && *nm) trackNames[idx] = nm;
			}
		}

		// Point the session at the restored folder (this clears its model), then re-seed it.
		resolvedSessionDir = loadDir;
		session.setDir(loadDir);
		appliedSessionBase = sessionBase;
		sessionRestored = true;

		json_t* slots = json_object_get(root, "slots");
		if (json_is_array(slots)) {
			size_t i; json_t* v;
			json_array_foreach(slots, i, v) {
				const char* file = json_string_value(json_object_get(v, "file"));
				int track = (int) json_integer_value(json_object_get(v, "track"));
				int slot = (int) json_integer_value(json_object_get(v, "slot"));
				if (track < 0 || track >= TRACKS || slot < 0 || slot >= SLOTS) continue;
				akaudio::looper::TakeMeta meta{};
				meta.frames = (int) json_integer_value(json_object_get(v, "frames"));
				meta.sampleRate = (float) json_number_value(json_object_get(v, "sampleRate"));
				meta.bpm = (int) json_integer_value(json_object_get(v, "bpm"));
				meta.bpi = (int) json_integer_value(json_object_get(v, "bpi"));
				meta.startFrame = (uint64_t) json_integer_value(json_object_get(v, "startFrame"));
				meta.peak = (float) json_number_value(json_object_get(v, "peak"));
				meta.repeats = (int) json_integer_value(json_object_get(v, "repeats"));
				meta.decayDb = (float) json_number_value(json_object_get(v, "decayDb"));
				meta.followSlot = (int) json_integer_value(json_object_get(v, "follow")); // absent (old manifest) → 0 = stop
				// The manifest is hand-editable: clamp what the engine consumes. A
				// positive decayDb would GROW the gain each wrap without bound (the
				// POLY direct outs have no limiter); out-of-range follow just clutters.
				meta.repeats = std::max(0, meta.repeats);
				meta.decayDb = clamp(meta.decayDb, -60.f, 0.f);
				meta.followSlot = clamp(meta.followSlot, 0, SLOTS);
				// Seed the slot's settings atomics right away (for take rows too, not
				// just settings-only ones): the widget's settings sweep runs before the
				// decoded take installs, and must see the restored values — not the
				// defaults — or it would clobber the just-seeded manifest entries.
				{
					akaudio::looper::Slot& sl = engine.tracks[track].slots[slot];
					sl.repeats.store(meta.repeats, std::memory_order_relaxed);
					sl.decayDb.store(meta.decayDb, std::memory_order_relaxed);
					sl.followSlot.store(meta.followSlot, std::memory_order_relaxed);
				}
				// Settings-only row (no file): an empty "rest" cell in a follow chain —
				// the seeding above is all it needs, nothing to load.
				if (!file || !*file) continue;
				if (meta.frames <= 0) continue;
				session.noteExistingTake(track, slot, file, meta);
				session.enqueueLoad(track, slot, loadDir + "/" + file, meta);
			}
		}
		json_decref(root);
		// The restored session.json exists on disk, so later settings/name edits (and
		// clears) may rewrite it — without this, flush()/clear() would refuse until a
		// brand-new take lands, and a reloaded patch would never persist edits.
		session.markRestored();
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "simSecondsIdx", json_integer(simSecondsIdx.load()));
		// Session folder (templated with ~ so a shared patch carries no user name). The audio
		// isn't in the patch — but the resolved session dir is, so a reload restores the grid
		// from the OGGs + session.json on disk (the clip loader).
		json_object_set_new(root, "sessionBase", json_string(akaudio::collapseHome(sessionBase).c_str()));
		if (!resolvedSessionDir.empty())
			json_object_set_new(root, "sessionDir", json_string(akaudio::collapseHome(resolvedSessionDir).c_str()));
		json_object_set_new(root, "defRepeats", json_integer(engine.defRepeats.load()));
		json_object_set_new(root, "defDecayDb", json_real(engine.defDecayDb.load()));
		json_object_set_new(root, "autoAdvance", json_boolean(engine.autoAdvance.load()));
		json_object_set_new(root, "followMixerNames", json_boolean(followMixerNames));
		json_t* names = json_array();
		for (int t = 0; t < TRACKS; t++)
			json_array_append_new(names, json_string(trackNames[t].c_str()));
		json_object_set_new(root, "trackNames", names);
		json_object_set_new(root, "embedTakes", json_boolean(embedTakes));
		return root;
	}
	void dataFromJson(json_t* root) override {
		json_t* j = json_object_get(root, "simSecondsIdx");
		if (j)
			simSecondsIdx.store(clamp((int) json_integer_value(j), 0, N_SIM_SECONDS - 1));
		j = json_object_get(root, "sessionBase");
		if (json_is_string(j)) {
			const char* b = json_string_value(j);
			if (b && *b) sessionBase = akaudio::expandHome(b);
		}
		// Restore the grid from the saved session folder on the first UI step (clip loader).
		j = json_object_get(root, "sessionDir");
		if (json_is_string(j)) {
			const char* d = json_string_value(j);
			if (d && *d) { loadDir = akaudio::expandHome(d); loadPending = true; }
		}
		j = json_object_get(root, "defRepeats");
		if (j) engine.defRepeats.store((int) json_integer_value(j));
		j = json_object_get(root, "defDecayDb");
		if (j) engine.defDecayDb.store((float) json_number_value(j));
		j = json_object_get(root, "autoAdvance");
		if (json_is_boolean(j)) engine.autoAdvance.store(json_boolean_value(j));
		j = json_object_get(root, "followMixerNames");
		if (json_is_boolean(j)) followMixerNames = json_boolean_value(j);
		j = json_object_get(root, "trackNames");
		if (json_is_array(j)) {
			for (int t = 0; t < TRACKS && t < (int) json_array_size(j); t++) {
				const char* n = json_string_value(json_array_get(j, t));
				if (n && *n) trackNames[t] = n;
			}
		}
		j = json_object_get(root, "embedTakes");
		if (json_is_boolean(j)) embedTakes = json_boolean_value(j);
	}

	// ---- Embed takes in patch (Rack patch storage; see SessionMirror.hpp) ----

	static const int SNAPSHOT_VERSION = 1;

	// A snapshot we don't recognize (a future format) is skipped, never guessed at.
	static bool snapshotVersionOk(const std::string& snapDir) {
		json_error_t err;
		json_t* root = json_load_file((snapDir + "/snapshot.json").c_str(), 0, &err);
		if (!root) return false;
		bool ok = json_integer_value(json_object_get(root, "version")) <= SNAPSHOT_VERSION;
		json_decref(root);
		return ok;
	}

	static void writeSnapshotMeta(const std::string& snapDir) {
		json_t* root = json_object();
		json_object_set_new(root, "version", json_integer(SNAPSHOT_VERSION));
		(void) json_dump_file(root, (snapDir + "/snapshot.json").c_str(), JSON_INDENT(2));
		json_decref(root);
	}

	// Called before every patch serialization (including autosave every ~15 s): mirror
	// the live grid into the patch. Incremental and mtime-preserving — an unchanged
	// grid writes nothing, and no patch-storage dir is created before the first real
	// take reaches disk (an empty session has no session.json).
	void onSave(const SaveEvent& e) override {
		Module::onSave(e);
		try {
			if (embedTakes) {
				if (resolvedSessionDir.empty() || !system::isFile(resolvedSessionDir + "/session.json"))
					return;
				std::string snap = createPatchStorageDirectory();
				int changes = akaudio::looper::mirrorSession(resolvedSessionDir, snap);
				if (changes > 0 || (changes == 0 && !system::isFile(snap + "/snapshot.json")))
					writeSnapshotMeta(snap);
			} else {
				// Opt-out also retracts an existing snapshot so the patch carries no stale audio.
				std::string snap = getPatchStorageDirectory();
				if (system::isDirectory(snap)) {
					akaudio::looper::clearSessionMirror(snap);
					system::remove(snap + "/snapshot.json");
				}
			}
		} catch (Exception& ex) {
			WARN("Looper: takes not embedded in patch: %s", ex.what());
		}
	}

	// The saved session folder is gone: rebuild it from the patch's snapshot, then let
	// the normal clip loader run against it (one input format — a session folder).
	void hydrateFromSnapshot() {
		try {
			std::string snap = getPatchStorageDirectory();
			if (!system::isFile(snap + "/session.json") || !snapshotVersionOk(snap))
				return;
			std::string base = akaudio::expandHome(sessionBase);
			if (ownSessionFolder.empty())
				ownSessionFolder = akaudio::timeStamp("%Y-%m-%d_%H%M") + "_session";
			std::string dir = base + "/" + ownSessionFolder + "/looper";
			if (akaudio::looper::mirrorSession(snap, dir) < 0)
				return;
			loadDir = dir;
		} catch (Exception& ex) {
			WARN("Looper: snapshot hydration skipped: %s", ex.what());
		}
	}
};

// =====================================================================================
// Widgets
// =====================================================================================

// Code-drawn panel decoration (NanoSVG ignores <text>): title, section labels, the
// MIX output plate, and the AK mark.
struct LooperDecor : Widget {
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		drawTxt(vg, FONT_BOLD, 10.f, 21.f, 15.f, lpText(), "LOOPER");
		// INS: the poly instrument input (its jack is a PolyPort with the gold collar ring).
		drawTxt(vg, FONT_BOLD, SCENE_X + SCENE_W / 2, NAME_Y + NAME_H / 2, 9.f, lpText(), "INS", NVG_ALIGN_CENTER);

		// ---- Controls column: Radio/Ninjam plate geometry (Theme.hpp) ----
		const NVGcolor bd = nvgRGBA(0, 0, 0, 0x55);
		drawTxt(vg, FONT_BOLD, RX, Y_DUB - 20.f, 11.f, lpText(), "OVERDUB", NVG_ALIGN_CENTER);
		// Three stacked plates: OUTS (poly per-track out) over CUE (cyan) over MIX.
		auto plateBox = [&](float top, float h) {
			nvgBeginPath(vg);
			nvgRoundedRect(vg, RX - PLATE_W / 2, top, PLATE_W, h, mm2px(AK_PLATE_R_MM));
			nvgFillColor(vg, akPlate());
			nvgFill(vg);
			nvgStrokeColor(vg, bd);
			nvgStrokeWidth(vg, 1.f);
			nvgStroke(vg);
		};
		auto plate = [&](float top, const char* title, NVGcolor titleCol) {
			plateBox(top, PLATE_H);
			drawTxt(vg, FONT_BOLD, RX, top + PLATE_TITLE_DY, 11.f, titleCol, title, NVG_ALIGN_CENTER);
			drawTxt(vg, FONT_BOLD, PLATE_LAB_X, plateLY(top), 11.f, akPlateText(), "L", NVG_ALIGN_CENTER);
			drawTxt(vg, FONT_BOLD, PLATE_LAB_X, plateRY(top), 11.f, akPlateText(), "R", NVG_ALIGN_CENTER);
		};
		// OUTS: a compact single-jack poly plate (per-track direct out); its jack is a
		// PolyPort (gold collar ring).
		plateBox(OUTS_TOP, OUTS_H);
		drawTxt(vg, FONT_BOLD, RX, OUTS_TOP + PLATE_TITLE_DY, 11.f, akPlateText(), "OUTS", NVG_ALIGN_CENTER);
		plate(CUE_TOP, "CUE", akCueCyan());
		plate(MIX_TOP, "MIX", akPlateText());
		// "AK" maker mark at the bottom, where Radio/Ninjam put it.
		drawTxt(vg, FONT_BOLD, box.size.x / 2.f, mm2px(AK_MARK_Y_MM), 16.f, lpText(), "AK", NVG_ALIGN_CENTER);
		Widget::draw(args);
	}
};

// Header status line (right side): clock source + countdown + selection.
struct HeaderStatus : Widget {
	Looper* lp = nullptr;
	void draw(const DrawArgs& args) override {
		std::string s;
		if (!lp) {
			s = "UX scaffold";
		} else {
			int sel = lp->selected.load(std::memory_order_relaxed);
			if (lp->ninjamClock.load(std::memory_order_relaxed))
				s = string::f("NINJAM \xc2\xb7 %d BPM \xc2\xb7 %d BPI \xc2\xb7 next in %.1f s",
					lp->clockBpm.load(std::memory_order_relaxed), lp->clockBpi.load(std::memory_order_relaxed),
					lp->secsLeft.load(std::memory_order_relaxed));
			else
				s = string::f("SIMULATED CLOCK \xc2\xb7 %d s interval \xc2\xb7 next in %.1f s",
					SIM_SECONDS[lp->simSecondsIdx.load(std::memory_order_relaxed)],
					lp->secsLeft.load(std::memory_order_relaxed));
			int pc = lp->pendingCount();
			if (pc) s += string::f(" \xc2\xb7 %d queued", pc);
			if (sel >= 0) s += string::f(" \xc2\xb7 sel %d.%d", sel / SLOTS + 1, sel % SLOTS + 1);
		}
		drawTxt(args.vg, FONT_BOLD, box.size.x, box.size.y / 2, 9.f, lpTextDim(), s, NVG_ALIGN_RIGHT);
		// Sync LED: green = locked to a Ninjam clock; dim = simulated / none.
		if (lp) {
			float tw = textWidth(args.vg, FONT_BOLD, 9.f, s);
			bool on = lp->ninjamClock.load(std::memory_order_relaxed);
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, box.size.x - tw - 10.f, box.size.y / 2, 4.f);
			nvgFillColor(args.vg, on ? lpGreen() : akShade(0x30));
			nvgFill(args.vg);
		}
		// Interval progress bar along the bottom edge of the header.
		if (lp) {
			float ph = lp->phase.load(std::memory_order_relaxed);
			nvgBeginPath(args.vg);
			nvgRect(args.vg, 0, box.size.y - 2.f, box.size.x * ph, 2.f);
			nvgFillColor(args.vg, lpGreen());
			nvgFill(args.vg);
		}
	}
};

// Inline editor for a track label, opened in a small menu (same pattern as Ninjam's
// display-name chip). Applies on every keystroke; Enter/Escape closes the menu.
struct TrackNameField : ui::TextField {
	Looper* lp = nullptr;
	int t = 0;
	TrackNameField() { box.size.x = 140.f; }
	void onChange(const ChangeEvent& e) override {
		if (lp) lp->trackNames[t] = text;
		ui::TextField::onChange(e);
	}
	void onSelectKey(const SelectKeyEvent& e) override {
		if (e.action == GLFW_PRESS && (e.key == GLFW_KEY_ENTER || e.key == GLFW_KEY_KP_ENTER
		        || e.key == GLFW_KEY_ESCAPE)) {
			ui::MenuOverlay* overlay = getAncestorOfType<ui::MenuOverlay>();
			if (overlay) overlay->requestDelete();
			e.consume(this);
			return;
		}
		ui::TextField::onSelectKey(e);
	}
};

// MindMeld-style track label: dark box, amber monospace text; click to edit.
struct TrackLabel : HoverButton {
	Looper* lp = nullptr;
	int t = 0;
	void onPress(const ButtonEvent& e) override {
		if (!lp) { e.consume(this); return; }
		ui::Menu* menu = createMenu();
		menu->addChild(createMenuLabel(string::f("Track %d name", t + 1)));
		TrackNameField* f = new TrackNameField;
		f->lp = lp; f->t = t;
		f->text = lp->trackNames[t];
		menu->addChild(f);
		Looper* m = lp; const int tt = t;
		menu->addChild(createMenuItem("Reset to default", "", [m, tt]() {
			m->trackNames[tt] = Looper::defaultTrackName(tt);
		}));
		APP->event->setSelectedWidget(f);
		f->selectAll();
		e.consume(this);
	}
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		const float w = box.size.x, h = box.size.y;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, w, h, 2.5f);
		nvgFillColor(vg, lpLabelBg());
		nvgFill(vg);
		if (hovered) {
			nvgStrokeColor(vg, nvgRGBA(0xff, 0xff, 0xff, 0x40));
			nvgStrokeWidth(vg, 1.f);
			nvgStroke(vg);
		}
		std::string name = lp ? lp->trackNames[t] : Looper::defaultTrackName(t);
		drawTxt(vg, FONT_MONO, w / 2, h / 2 + 0.5f, 11.f, lpLabelText(), name, NVG_ALIGN_CENTER, w - 6.f);
		// Source tag (right edge): J = own jack, P3-4 = MULTI channels, nothing = no input.
		if (lp) {
			int src = lp->trackSource[t].load(std::memory_order_relaxed);
			int mch = lp->trackMultiCh[t].load(std::memory_order_relaxed);
			float lvl = lp->trackLevel[t].load(std::memory_order_relaxed);
			std::string tag = src == 1 ? "J" : src == 2 ? string::f("P%d-%d", mch + 1, mch + 2) : "";
			if (!tag.empty()) {
				bool sig = lvl > 0.003f; // ~−50 dBFS: audio is actually arriving on this track
				NVGcolor c = sig ? nvgRGB(0x3a, 0xd0, 0x6a) : nvgTransRGBAf(lpLabelText(), 0.55f);
				drawTxt(vg, FONT_BOLD, w - 2.f, h / 2 + 0.5f, 6.5f, c, tag, NVG_ALIGN_RIGHT);
			}
		}
	}
};

// Hover tracking shared by the scaffold's param buttons.
struct HoverSwitch : app::Switch {
	bool hovered = false;
	void onEnter(const EnterEvent& e) override { hovered = true; app::Switch::onEnter(e); }
	void onLeave(const LeaveEvent& e) override { hovered = false; app::Switch::onLeave(e); }
};

// A clip slot: rectangular, thumbnail inside, state by fill/outline, settings tag.
struct SlotButton : HoverSwitch {
	Looper* lp = nullptr;
	int t = 0, s = 0;
	SlotButton() { momentary = true; }

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		const float w = box.size.x, h = box.size.y;
		int state = akaudio::looper::EMPTY, pending = akaudio::looper::NONE, reps = 0, repCount = 0;
		int follow = 0;
		float decay = 0.f, gain = 1.f;
		bool sel = false, playable = true, overdubbing = false;
		const float* thumb = nullptr;
		float preview[THUMB_BINS];
		if (lp) {
			akaudio::looper::Slot& sl = lp->engine.tracks[t].slots[s];
			state = sl.state.load(std::memory_order_relaxed);
			pending = sl.pending.load(std::memory_order_relaxed);
			reps = sl.repeats.load(std::memory_order_relaxed);
			repCount = sl.repCount.load(std::memory_order_relaxed);
			follow = sl.followSlot.load(std::memory_order_relaxed);
			decay = sl.decayDb.load(std::memory_order_relaxed);
			gain = sl.gain.load(std::memory_order_relaxed);
			sel = lp->selected.load(std::memory_order_relaxed) == t * SLOTS + s;
			playable = sl.playable.load(std::memory_order_relaxed);
			overdubbing = sl.overdubbing.load(std::memory_order_relaxed);
			thumb = sl.thumb;
		} else if ((t * 3 + s * 5) % 7 < 2) {
			// Library/browser preview: a few fake clips so the panel reads as a looper.
			state = (t + s) % 3 == 0 ? akaudio::looper::PLAYING : akaudio::looper::FILLED;
			for (int i = 0; i < THUMB_BINS; i++)
				preview[i] = 0.15f + 0.8f * std::fabs(std::sin(0.37f * i + t + s)) * std::exp(-0.04f * (i % 16));
			thumb = preview;
		}

		// Body.
		NVGcolor bg = state == akaudio::looper::EMPTY ? lpWell()
		            : state == akaudio::looper::PLAYING ? nvgTransRGBAf(lpGreen(), 0.22f)
		            : state == akaudio::looper::RECORDING ? nvgTransRGBAf(lpRed(), 0.15f)
		            : lpCard();
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, w, h, 3.f);
		nvgFillColor(vg, bg);
		nvgFill(vg);
		nvgStrokeColor(vg, lpBorder());
		nvgStrokeWidth(vg, 1.f);
		nvgStroke(vg);

		// Thumbnail (a take whose length no longer matches the grid draws faint: not launchable).
		if ((state == akaudio::looper::FILLED || state == akaudio::looper::PLAYING) && thumb) {
			const float bw = (w - 4.f) / THUMB_BINS;
			NVGcolor c = state == akaudio::looper::PLAYING ? lpGreen() : lpTextDim();
			if (!playable) c = nvgTransRGBAf(c, 0.3f);
			nvgFillColor(vg, state == akaudio::looper::PLAYING ? nvgTransRGBAf(c, 0.35f + 0.65f * gain) : c);
			for (int i = 0; i < THUMB_BINS; i++) {
				float bh = std::max(1.f, thumb[i] * (h - 6.f));
				nvgBeginPath(vg);
				nvgRect(vg, 2.f + i * bw, h / 2 - bh / 2, std::max(0.5f, bw - 0.3f), bh);
				nvgFill(vg);
			}
		}
		// Recording (or overdubbing): the interval being recorded fills the slot
		// left → right, like a recording clip — red while recording, amber over the
		// take for overdub. The bins come from the track's rolling recorder.
		if (lp && (state == akaudio::looper::RECORDING || overdubbing)) {
			akaudio::looper::Track& tr = lp->engine.tracks[t];
			float ph = lp->phase.load(std::memory_order_relaxed);
			const float bw = (w - 4.f) / THUMB_BINS;
			int upto = (int) (ph * THUMB_BINS);
			bool present = tr.present.load(std::memory_order_relaxed);
			nvgFillColor(vg, state == akaudio::looper::RECORDING ? lpRed() : lpAmber());
			for (int i = 0; i <= upto && i < THUMB_BINS; i++) {
				float a = present ? tr.live[i] : 0.08f;
				float bh = std::max(1.f, a * (h - 6.f));
				nvgBeginPath(vg);
				nvgRect(vg, 2.f + i * bw, h / 2 - bh / 2, std::max(0.5f, bw - 0.3f), bh);
				nvgFill(vg);
			}
		}
		// Playhead.
		if (state == akaudio::looper::PLAYING) {
			float ph = lp ? lp->phase.load(std::memory_order_relaxed) : 0.4f;
			nvgBeginPath(vg);
			nvgRect(vg, 2.f + ph * (w - 4.f) - 0.5f, 1.f, 1.f, h - 2.f);
			nvgFillColor(vg, lpText());
			nvgFill(vg);
		}
		// Settings tag (top-right): ∞ / ×N / N left, plus ↘ when decaying, →N when chained.
		// An EMPTY cell shows it only when a follow action makes it a "rest" step.
		// A translucent scrim backs the text so it stays legible over thumbnails.
		if (state == akaudio::looper::FILLED || state == akaudio::looper::PLAYING
		        || (state == akaudio::looper::EMPTY && follow > 0)) {
			std::string tag;
			if (reps == 0) tag = "\xe2\x88\x9e";
			else if (state == akaudio::looper::PLAYING) tag = string::f("%d", std::max(0, reps - repCount));
			else tag = string::f("\xc3\x97%d", reps);
			if (decay < -0.01f) tag += "\xe2\x86\x98";
			if (follow > 0) tag += string::f("\xe2\x86\x92%d", follow);
			std::shared_ptr<window::Font> tf = akLoadFont(FONT_BOLD);
			if (tf && tf->handle >= 0) {
				nvgFontFaceId(vg, tf->handle);
				nvgFontSize(vg, 10.f);
				nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
				float tb[4];
				nvgTextBounds(vg, 0, 0, tag.c_str(), NULL, tb);
				const float tw = tb[2] - tb[0];
				nvgBeginPath(vg);
				nvgRoundedRect(vg, w - 4.f - tw - 3.f, 1.5f, tw + 5.f, 11.f, 3.f);
				nvgFillColor(vg, akTheme(nvgRGBA(0xeb, 0xeb, 0xeb, 200), nvgRGBA(0x16, 0x18, 0x1a, 200)));
				nvgFill(vg);
			}
			drawTxt(vg, FONT_BOLD, w - 4.f, 7.f, 10.f, lpText(), tag, NVG_ALIGN_RIGHT);
		}
		// Armed: blinking outline colored by the queued operation; a steady amber outline
		// marks the cell that is actively overdubbing (continuous, not a queued action).
		if (pending != akaudio::looper::NONE || overdubbing) {
			double tm = system::getTime();
			float a = overdubbing ? 0.9f
			                      : 0.45f + 0.55f * (float) (0.5 + 0.5 * std::sin(tm * 2.0 * M_PI * 2.5));
			NVGcolor c = overdubbing ? lpAmber()
			           : pending == akaudio::looper::CAPTURE ? lpRed()
			           : pending == akaudio::looper::LAUNCH ? lpGreen()
			           : lpTextDim();
			nvgBeginPath(vg);
			nvgRoundedRect(vg, 1.f, 1.f, w - 2.f, h - 2.f, 2.5f);
			nvgStrokeColor(vg, nvgTransRGBAf(c, a));
			nvgStrokeWidth(vg, 2.f);
			nvgStroke(vg);
		}
		// Refused capture: red flash.
		if (lp) {
			double f = lp->engine.tracks[t].slots[s].flashAt.load(std::memory_order_relaxed);
			double dt = system::getTime() - f;
			if (f > 0 && dt < 0.5) {
				nvgBeginPath(vg);
				nvgRoundedRect(vg, 0, 0, w, h, 3.f);
				nvgFillColor(vg, nvgTransRGBAf(lpRed(), (float) (0.6 * (1.0 - dt / 0.5))));
				nvgFill(vg);
			}
		}
		// Selected ring.
		if (sel) {
			nvgBeginPath(vg);
			nvgRoundedRect(vg, -1.5f, -1.5f, w + 3.f, h + 3.f, 4.f);
			nvgStrokeColor(vg, lpRing());
			nvgStrokeWidth(vg, 1.5f);
			nvgStroke(vg);
		}
		if (hovered) {
			nvgBeginPath(vg);
			nvgRoundedRect(vg, 0, 0, w, h, 3.f);
			nvgFillColor(vg, akShade(0x18));
			nvgFill(vg);
		}
	}

	void appendContextMenu(ui::Menu* menu) override {
		if (!lp) return;
		Looper* m = lp;
		const int tt = t, ss = s;
		akaudio::looper::Slot& sl = lp->engine.tracks[t].slots[s];
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel(string::f("Track %d \xc2\xb7 slot %d", t + 1, s + 1)));
		akaudio::looper::Slot* slp = &sl;
		menu->addChild(createIndexSubmenuItem("Repeats",
			repeatLabels(),
			[slp]() { return (size_t) repeatsIndex(slp->repeats.load(std::memory_order_relaxed)); },
			[slp, m, tt, ss](size_t i) {
				slp->repeats.store(REPEAT_CHOICES[i], std::memory_order_relaxed);
				m->pushSlotSettings(tt, ss);
			}));
		menu->addChild(createIndexSubmenuItem("Decay per repetition",
			decayLabels(),
			[slp]() { return (size_t) decayIndex(slp->decayDb.load(std::memory_order_relaxed)); },
			[slp, m, tt, ss](size_t i) {
				slp->decayDb.store(DECAY_CHOICES[i], std::memory_order_relaxed);
				m->pushSlotSettings(tt, ss);
			}));
		// After the clip is done (repeats exhausted / decayed out): stop, or chain on.
		std::vector<std::string> after;
		after.push_back("Stop");
		for (int i = 0; i < SLOTS; i++)
			after.push_back(i == ss ? string::f("Play slot %d (this: retrigger)", i + 1)
			                        : string::f("Play slot %d", i + 1));
		menu->addChild(createIndexSubmenuItem("After", after,
			[slp]() { return (size_t) slp->followSlot.load(std::memory_order_relaxed); },
			[slp, m, tt, ss](size_t i) {
				slp->followSlot.store((int) i, std::memory_order_relaxed); // 0 = stop, i = slot i (1-based)
				m->pushSlotSettings(tt, ss);
			}));
		menu->addChild(createMenuItem("Clear slot", "", [m, tt, ss]() { m->clearSlot(tt, ss); },
			sl.state.load(std::memory_order_relaxed) == akaudio::looper::EMPTY));
	}
};

// Per-track TRANSMIT latch: Ninjam's TX LED (akDrawTxLed), as a param so it's
// MIDI-mappable and saved with the patch. Lit = live input on air; dark = private.
struct TxSwitch : HoverSwitch {
	void draw(const DrawArgs& args) override {
		ParamQuantity* pq = getParamQuantity();
		bool on = pq ? pq->getValue() > 0.5f : true; // on air (MIX) vs private (CUE)
		const float cx = box.size.x / 2, cy = box.size.y / 2, r = 5.5f;
		// Always lit: green = transmitting to MIX, cyan = routed to CUE (private).
		if (on) akDrawTxLedC(args.vg, cx, cy, r, true, hovered, akTxGreen(), nvgRGB(0x4a, 0xe0, 0x82));
		else    akDrawTxLedC(args.vg, cx, cy, r, true, hovered, akCueCyan(), nvgRGB(0x62, 0xdc, 0xf0));
	}
};

// Small glyph buttons: scene ▶, track stop ■, stop-all.
struct GlyphButton : HoverSwitch {
	enum Kind { SCENE, STOP, STOP_ALL } kind = SCENE;
	Looper* lp = nullptr;
	int idx = 0;
	bool sceneHasPending() {
		if (!lp || kind != SCENE) return false;
		for (int t = 0; t < TRACKS; t++)
			if (lp->engine.tracks[t].slots[idx].pending.load(std::memory_order_relaxed) != akaudio::looper::NONE) return true;
		return false;
	}
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		const float w = box.size.x, h = box.size.y;
		const float r = std::min(6.f, h / 2.f);
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, w, h, r);
		nvgFillColor(vg, hovered ? lpButtonHi() : lpButton());
		nvgFill(vg);
		// Soft top highlight so the button reads as raised.
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 1.f, 1.f, w - 2.f, h / 2.f, r);
		nvgFillColor(vg, akDark() ? nvgRGBA(0xff, 0xff, 0xff, 0x10) : nvgRGBA(0xff, 0xff, 0xff, 0x55));
		nvgFill(vg);
		NVGcolor ink = lpText();
		const float cx = w / 2, cy = h / 2;
		switch (kind) {
			case SCENE: {
				nvgBeginPath(vg);
				nvgMoveTo(vg, cx - 3.f, cy - 4.f);
				nvgLineTo(vg, cx + 4.f, cy);
				nvgLineTo(vg, cx - 3.f, cy + 4.f);
				nvgClosePath(vg);
				nvgFillColor(vg, sceneHasPending() ? lpGreen() : ink);
				nvgFill(vg);
				break;
			}
			case STOP:
			case STOP_ALL: {
				float sz = kind == STOP_ALL ? 8.f : 7.f;
				nvgBeginPath(vg);
				nvgRect(vg, cx - sz / 2, cy - sz / 2, sz, sz);
				nvgFillColor(vg, ink);
				nvgFill(vg);
				break;
			}
		}
	}
};

// MindMeld-style poly port: the standard PJ301M with a gold ring drawn inside the jack
// (between the centre hole and the collar), so a poly I/O (the INS input, the OUTS
// per-track output) reads as poly at a glance — matching how MindMeld colours the ring in
// the jack, not a halo around it. Colours + proportions taken from MindMeldModular's
// res/comp/jack-poly.svg (© 2019-2023 Marc Boulé & Steve Baker, GPL-3): the gold is a
// vertical gradient #af9420→#99841c filling an annulus at 0.45–0.64 of the port radius
// (their hole r=5.1, gold r=7.3 in an 11.34-radius jack).
struct PolyPort : ThemedPJ301MPort {
	void draw(const DrawArgs& args) override {
		ThemedPJ301MPort::draw(args);
		NVGcontext* vg = args.vg;
		const float R = box.size.x / 2.f, cx = R, cy = box.size.y / 2.f;
		const float outerR = R * 0.6437f, innerR = R * 0.4497f;
		const float midR = (outerR + innerR) / 2.f, w = outerR - innerR;
		NVGpaint gold = nvgLinearGradient(vg, cx, cy - outerR, cx, cy + outerR,
			nvgRGB(0xaf, 0x94, 0x20), nvgRGB(0x99, 0x84, 0x1c));
		nvgBeginPath(vg);
		nvgCircle(vg, cx, cy, midR);
		nvgStrokeWidth(vg, w);
		nvgStrokePaint(vg, gold);
		nvgStroke(vg);
	}
};

struct LooperWidget : ModuleWidget {
	Looper* lp = nullptr;

	explicit LooperWidget(Looper* module) {
		lp = module;
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Looper.svg"),
			asset::plugin(pluginInstance, "res/Looper-dark.svg")));

		addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		LooperDecor* decor = new LooperDecor;
		decor->box.size = box.size;
		addChild(decor);

		HeaderStatus* hs = new HeaderStatus;
		hs->lp = module;
		hs->box.pos = Vec(100.f, 8.f);
		hs->box.size = Vec(box.size.x - 100.f - 10.f, 26.f);
		addChild(hs);

		for (int t = 0; t < TRACKS; t++) {
			TrackLabel* lab = new TrackLabel;
			lab->lp = module; lab->t = t;
			lab->box.pos = Vec(colX(t) + 2.f, NAME_Y);
			lab->box.size = Vec(BTN_W, NAME_H);
			addChild(lab);
		}

		for (int t = 0; t < TRACKS; t++) {
			addInput(createInputCentered<ThemedPJ301MPort>(Vec(colCx(t) - JACK_DX, JACK_Y), module, Looper::IN_L_INPUT + t));
			addInput(createInputCentered<ThemedPJ301MPort>(Vec(colCx(t) + JACK_DX, JACK_Y), module, Looper::IN_R_INPUT + t));
			for (int s = 0; s < SLOTS; s++) {
				SlotButton* b = createParam<SlotButton>(Vec(colX(t) + 2.f, GRID_Y + s * ROW_H), module,
					Looper::SLOT_PARAM + t * SLOTS + s);
				b->box.size = Vec(BTN_W, BTN_H);
				b->lp = module; b->t = t; b->s = s;
				addParam(b);
			}
			// Stop row: [■ stop][● TX] — the LED is Ninjam's transmit LED.
			GlyphButton* st = createParam<GlyphButton>(Vec(colX(t) + 2.f, STOP_Y), module, Looper::STOP_PARAM + t);
			st->box.size = Vec(BTN_W - 20.f, stopH()); st->kind = GlyphButton::STOP; st->lp = module; st->idx = t; st->momentary = true;
			addParam(st);
			TxSwitch* tx = createParam<TxSwitch>(Vec(colX(t) + 2.f + BTN_W - 18.f, STOP_Y), module, Looper::TX_PARAM + t);
			tx->box.size = Vec(18.f, stopH());
			addParam(tx);
		}
		addInput(createInputCentered<PolyPort>(Vec(SCENE_X + SCENE_W / 2, JACK_Y), module, Looper::MULTI_INPUT));
		for (int s = 0; s < SLOTS; s++) {
			GlyphButton* sc = createParam<GlyphButton>(Vec(SCENE_X, GRID_Y + s * ROW_H), module, Looper::SCENE_PARAM + s);
			sc->box.size = Vec(SCENE_W, BTN_H); sc->kind = GlyphButton::SCENE; sc->lp = module; sc->idx = s; sc->momentary = true;
			addParam(sc);
		}
		GlyphButton* sa = createParam<GlyphButton>(Vec(SCENE_X, STOP_Y), module, Looper::STOP_ALL_PARAM);
		sa->box.size = Vec(SCENE_W, stopH()); sa->kind = GlyphButton::STOP_ALL; sa->lp = module; sa->momentary = true;
		addParam(sa);

		// Bottom I/O strip.
		addOutput(createOutputCentered<PolyPort>(Vec(RX, OUTS_JACK_Y), module, Looper::POLY_OUTPUT));
		addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(PLATE_JACK_X, plateLY(CUE_TOP)), module, Looper::CUE_L_OUTPUT));
		addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(PLATE_JACK_X, plateRY(CUE_TOP)), module, Looper::CUE_R_OUTPUT));
		addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(PLATE_JACK_X, plateLY(MIX_TOP)), module, Looper::MIX_L_OUTPUT));
		addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(PLATE_JACK_X, plateRY(MIX_TOP)), module, Looper::MIX_R_OUTPUT));
		// Overdub: the component-library bezel button with a light (what Fundamental's
		// PUSH uses), latching; red = overdub armed.
		addParam(createLightParamCentered<VCVLightBezelLatch<RedLight>>(Vec(RX, Y_DUB), module,
			Looper::OVERDUB_PARAM, Looper::OVERDUB_LIGHT));
	}

	void step() override {
		ModuleWidget::step();
		if (!lp) return;
		lp->syncSession(); // M4: keep the on-disk session folder + manifest current
		// Reconcile every cell's settings into the manifest about twice a second: menu
		// edits push directly, but the engine also rewires cells itself (auto-advance
		// chain wiring) and empty "rest" cells have settings worth persisting. Each
		// push is a mutex-guarded compare that no-ops when nothing changed.
		if (++settingsSweep >= 30) {
			settingsSweep = 0;
			// Tempo-derived cells (BPI tile/split) are RAM-only: their rewired settings
			// must not leak into the manifest, which keeps the ORIGINAL take's settings.
			for (int t = 0; t < TRACKS; t++)
				for (int s = 0; s < SLOTS; s++)
					if (!lp->engine.tracks[t].slots[s].derived.load(std::memory_order_relaxed))
						lp->pushSlotSettings(t, s);
			// The name sync serializes the whole MixMaster (dataToJson) — every 4th
			// sweep (~2 s) is plenty for label edits.
			if (lp->followMixerNames && ++nameSweep >= 4) {
				nameSweep = 0;
				syncMixerNames();
			}
		}
	}
	int settingsSweep = 0;
	int nameSweep = 3; // first name sync on the first sweep after enabling

	// Copy the track labels of the MindMeld MixMaster feeding our MULTI input over our
	// own (menu toggle "Track names: follow the mixer"). MixMaster stores all labels as
	// one string of 4-char chunks ("trackLabels" in its module JSON, tracks first);
	// its direct-out poly jack interleaves L/R exactly like MULTI, so labels map 1:1.
	// The 16-track MixMaster has two direct-out jacks — the connected port's own name
	// ("1-8" / "9-16") gives the offset. UI thread only (dataToJson allocates).
	void syncMixerNames() {
		Module* src = nullptr;
		int outId = -1;
		for (int64_t cid : APP->engine->getCableIds()) {
			const Cable* c = APP->engine->getCable(cid);
			if (c && c->inputModule == lp && c->inputId == Looper::MULTI_INPUT) {
				src = c->outputModule;
				outId = c->outputId;
				break;
			}
		}
		if (!src || !src->model || !src->model->plugin) return;
		if (src->model->plugin->slug != "MindMeldModular") return;
		const std::string& ms = src->model->slug;
		if (ms != "MixMasterJr" && ms != "MixMaster") return;
		json_t* j = src->dataToJson();
		if (!j) return;
		const char* s = json_string_value(json_object_get(j, "trackLabels"));
		if (s) {
			const std::string all = s;
			int off = 0;
			if (outId >= 0 && outId < (int) src->outputInfos.size() && src->outputInfos[outId]
			        && src->outputInfos[outId]->getName().find("9-16") != std::string::npos)
				off = 8;
			for (int t = 0; t < TRACKS; t++) {
				size_t pos = (size_t) (off + t) * 4;
				if (pos + 4 > all.size()) break;
				// Printable ASCII only: the byte-wise 4-char slice could split a
				// multi-byte UTF-8 character, and an invalid-UTF-8 name corrupts both
				// the patch JSON (json_string() rejects it, silently shifting every
				// later name) and session.json (which then fails to parse on reload,
				// killing the whole clip restore).
				std::string name;
				for (size_t i = pos; i < pos + 4; i++) {
					char ch = all[i];
					if ((ch >= 0x21 && ch <= 0x7E) || (ch == ' ' && !name.empty()))
						name += ch;
				}
				while (!name.empty() && name.back() == ' ') name.pop_back();
				if (!name.empty()) lp->trackNames[t] = name;
			}
		}
		json_decref(j);
	}

	void appendContextMenu(Menu* menu) override {
		Looper* m = lp;
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexSubmenuItem("Simulated interval (when no Ninjam clock)", {"2 s", "4 s", "8 s", "16 s", "32 s"},
			[m]() { return (size_t) m->simSecondsIdx.load(std::memory_order_relaxed); },
			[m](size_t i) { m->simSecondsIdx.store((int) i, std::memory_order_relaxed); }));
		menu->addChild(createIndexSubmenuItem("New clips: repeats",
			repeatLabels(),
			[m]() { return (size_t) repeatsIndex(m->engine.defRepeats.load(std::memory_order_relaxed)); },
			[m](size_t i) { m->engine.defRepeats.store(REPEAT_CHOICES[i], std::memory_order_relaxed); }));
		menu->addChild(createIndexSubmenuItem("New clips: decay per repetition",
			decayLabels(),
			[m]() { return (size_t) decayIndex(m->engine.defDecayDb.load(std::memory_order_relaxed)); },
			[m](size_t i) { m->engine.defDecayDb.store(DECAY_CHOICES[i], std::memory_order_relaxed); }));
		// Off = the escape hatch for never-silent sources (drones), where playing
		// through the downbeat would chain the capture down the whole column.
		menu->addChild(createBoolMenuItem("Capture: auto-advance while playing", "",
			[m]() { return m->engine.autoAdvance.load(std::memory_order_relaxed); },
			[m](bool v) { m->engine.autoAdvance.store(v, std::memory_order_relaxed); }));
		// While on, the MixMaster feeding MULTI names our tracks (overwriting local
		// renames — that's what following means). The widget sweep does the copying.
		menu->addChild(createBoolPtrMenuItem("Track names: follow the mixer", "",
			&m->followMixerNames));
		menu->addChild(createMenuItem("Clear all slots", "", [m]() {
			for (int t = 0; t < TRACKS; t++)
				for (int s = 0; s < SLOTS; s++)
					m->clearSlot(t, s);
			m->selected.store(-1, std::memory_order_relaxed);
		}));

		// Session files (M4): loops are saved as OGG under this folder, sharing a Recorder's
		// jam folder when one is armed. Takes survive a reload (the v2 loader restores them).
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Loops saved to: " + akaudio::collapseHome(m->sessionBase)));
		menu->addChild(createMenuItem("Choose folder\xe2\x80\xa6", "", [m]() {
			char* path = osdialog_file(OSDIALOG_OPEN_DIR, m->sessionBase.c_str(), NULL, NULL);
			if (path) { m->sessionBase = path; std::free(path); }
		}));
		menu->addChild(createMenuItem("Reset to ~/Music/jams", "", [m]() {
			m->sessionBase = akaudio::defaultJamsDir();
		}, m->sessionBase == akaudio::defaultJamsDir()));
		std::string cur = m->resolvedSessionDir;
		menu->addChild(createMenuItem("Open this session's folder", "", [cur]() {
			if (!cur.empty()) system::openDirectory(cur);
		}, cur.empty()));
		// Snapshot the live grid into the .vcv on save, so a shared patch (or one whose
		// jams folder is gone) reloads with its loops. The session folder stays canonical.
		menu->addChild(createBoolPtrMenuItem("Embed takes in patch", "", &m->embedTakes));
	}
};

Model* modelLooper = createModel<Looper, LooperWidget>("Looper");
