// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

// Looper — 8 tracks × 8 slots Ableton-Session-style interval looper for NINJAM jams.
// Design: docs/LOOPER_DESIGN.md. This is the **UX scaffold** (milestone "UX"): the
// complete panel and the real slot state machine (arm → commit at the boundary,
// scenes, stops, repeats/decay, selection, menus) driven by a SIMULATED interval
// clock, so the feel of queued actions can be settled before any engine exists.
// What is real: the armed slot's live fill and capture thumbnails come from the inputs,
// and the inputs pass through to MIX. What is not: no audio
// is stored or played back, and there is no Ninjam clock yet (JamClock, milestone M0).

#include "plugin.hpp"
#include "Theme.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>

namespace {

const int TRACKS = 8;
const int SLOTS = 8;
const int THUMB_BINS = 64;        // thumbnail / LIVE strip resolution
const float GATE = 0.000316f;     // −70 dBFS on the ±1 scale: a silent capture is refused

const int REPEAT_CHOICES[] = {0, 1, 2, 4, 8, 16, 32, 64}; // 0 = ∞ (REPEATS knob positions)
const int N_REPEAT_CHOICES = 8;
const float DECAY_CHOICES[] = {0.f, -1.f, -2.f, -3.f, -6.f}; // dB per repetition (menus)
const int N_DECAY_CHOICES = 5;
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
// Grid on the left, scene column beside it, then a controls column (DUB, REPEATS,
// DECAY, MIX) to the right. The output plates / input well reuse the
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
const float Y_DUB = 70.f;                      // overdub bezel button (label above, like Fundamental's PUSH)
const float Y_REPEATS = 116.f, Y_DECAY = 162.f; // knob centers (labels 17 px above)
// MIX plate: L over R on Ninjam's two output rows (MAIN row = AK_ROW_CV_MM, CV row =
// AK_ROW_OUT_MM), so the plate spans exactly Ninjam's output block and the jacks line
// up across the two modules. Labels left of the jacks.
inline float mixPlateTop() { return mm2px(AK_PLATE_TOP_MM - (AK_ROW_OUT_MM - AK_ROW_CV_MM)); }
inline float mixPlateBottom() { return mm2px(AK_PLATE_TOP_MM + AK_PLATE_H_MM); }
inline float mixJackY(int ch) { return mm2px(ch == 0 ? AK_ROW_CV_MM : AK_ROW_OUT_MM); }
const float MIX_JACK_X = RX + 7.f, MIX_LAB_X = RX - 13.f;
const float MIX_PLATE_W = 46.f;
inline float stopH() { return mixPlateBottom() - STOP_Y; }

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
		REPEATS_PARAM,
		DECAY_PARAM,
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
		OUTPUTS_LEN
	};
	enum LightId { OVERDUB_LIGHT, LIGHTS_LEN };

	enum SlotState { EMPTY = 0, FILLED = 1, PLAYING = 2 };
	enum Pending { NONE = 0, CAPTURE = 1, LAUNCH = 2, STOP = 3, OVERDUB = 4 };

	// Per-slot state. The state machine lives on the audio thread (where the button
	// edges are); the widget reads atomics. `thumb` is a plain array (display-only,
	// benign race; written only at a boundary).
	struct Slot {
		std::atomic<int> state{EMPTY};
		std::atomic<int> pending{NONE};
		std::atomic<int> repeats{0};       // 0 = ∞ (UI writes, process reads)
		std::atomic<float> decayDb{0.f};   // dB per repetition, 0…−6
		std::atomic<int> repCount{0};
		std::atomic<float> gain{1.f};
		std::atomic<double> flashAt{-1.0}; // wall time of a refused capture (red flash)
		bool startedThisBoundary = false;  // audio thread: skip the wrap on the commit boundary
		float thumb[THUMB_BINS] = {};
	};
	struct Track {
		Slot slots[SLOTS];
		std::atomic<int> playingSlot{-1};
		std::atomic<bool> present{false};
		float live[THUMB_BINS] = {};     // interval in progress (process writes; an armed slot draws it)
		float lastLive[THUMB_BINS] = {}; // the interval just completed (capture source)
		float peak = 0.f;                // peak of the interval in progress
		float lastPeak = 0.f;
		float txGain = 1.f;              // smoothed TX latch (process only): no click on toggle
	};
	Track tracks[TRACKS];

	// Editable track labels (UI thread only; persisted). Default "-01-" … "-08-".
	std::string trackNames[TRACKS];

	// Selection = the last pressed slot (or a menu "Select"). Index track*SLOTS+slot, −1 none.
	std::atomic<int> selected{-1};
	// Module-level defaults for new captures (context menu).
	std::atomic<int> defRepeats{0};
	std::atomic<float> defDecayDb{0.f};

	// ---- Simulated interval clock (scaffold only) ----
	std::atomic<int> simSecondsIdx{1};      // index into SIM_SECONDS (4 s)
	std::atomic<float> phase{0.f};          // 0..1 interval progress (UI)
	std::atomic<float> secsLeft{0.f};       // seconds to the next boundary (UI)
	int64_t frameInInterval = 0;            // process() only
	int64_t intervalFrames = 1;
	uint32_t lcg = 0x9e3779b9u;             // synthetic thumbnails for unpatched tracks

	dsp::BooleanTrigger slotTrig[TRACKS * SLOTS];
	dsp::BooleanTrigger sceneTrig[SLOTS];
	dsp::BooleanTrigger stopTrig[TRACKS];
	dsp::BooleanTrigger stopAllTrig;

	Looper() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		for (int t = 0; t < TRACKS; t++)
			trackNames[t] = defaultTrackName(t);
		for (int t = 0; t < TRACKS; t++) {
			for (int s = 0; s < SLOTS; s++)
				configButton(SLOT_PARAM + t * SLOTS + s, string::f("Track %d slot %d", t + 1, s + 1));
			configButton(STOP_PARAM + t, string::f("Stop track %d", t + 1));
			configSwitch(TX_PARAM + t, 0.f, 1.f, 1.f, string::f("Track %d transmit (live input to MIX)", t + 1),
				{"Private", "On air"});
			configInput(IN_L_INPUT + t, string::f("Track %d L", t + 1));
			configInput(IN_R_INPUT + t, string::f("Track %d R (unpatched = mono)", t + 1));
		}
		configInput(MULTI_INPUT, "Multi (poly: ch 1-2 = track 1 L/R, 3-4 = track 2 â¦; a track's own jack wins)");
		for (int s = 0; s < SLOTS; s++)
			configButton(SCENE_PARAM + s, string::f("Scene %d", s + 1));
		configButton(STOP_ALL_PARAM, "Stop all tracks");
		configSwitch(REPEATS_PARAM, 0.f, (float) (N_REPEAT_CHOICES - 1), 0.f, "Repeats (selected slot)",
			{"\xe2\x88\x9e", "1", "2", "4", "8", "16", "32", "64"});
		configParam(DECAY_PARAM, -6.f, 0.f, 0.f, "Decay (selected slot)", " dB/rep");
		configSwitch(OVERDUB_PARAM, 0.f, 1.f, 0.f, "Overdub mode", {"Off", "On"});
		configOutput(MIX_L_OUTPUT, "Mix L (to Ninjam IN)");
		configOutput(MIX_R_OUTPUT, "Mix R (to Ninjam IN)");
	}

	static std::string defaultTrackName(int t) { return string::f("-%02d-", t + 1); }

	Slot& slotAt(int idx) { return tracks[idx / SLOTS].slots[idx % SLOTS]; }

	// ---- Button intents (audio thread) ----
	void pressSlot(int t, int s) {
		Slot& sl = tracks[t].slots[s];
		selected.store(t * SLOTS + s, std::memory_order_relaxed);
		if (sl.pending.load(std::memory_order_relaxed) != NONE) {
			sl.pending.store(NONE, std::memory_order_relaxed); // press again = cancel
			return;
		}
		switch (sl.state.load(std::memory_order_relaxed)) {
			case EMPTY:   sl.pending.store(CAPTURE, std::memory_order_relaxed); break;
			case FILLED:  sl.pending.store(LAUNCH, std::memory_order_relaxed); break;
			case PLAYING:
				sl.pending.store(params[OVERDUB_PARAM].getValue() > 0.5f ? OVERDUB : STOP,
					std::memory_order_relaxed);
				break;
		}
	}
	void armStopTrack(int t) {
		int p = tracks[t].playingSlot.load(std::memory_order_relaxed);
		if (p >= 0)
			tracks[t].slots[p].pending.store(STOP, std::memory_order_relaxed);
	}
	void pressScene(int row) {
		for (int t = 0; t < TRACKS; t++) {
			Slot& sl = tracks[t].slots[row];
			switch (sl.state.load(std::memory_order_relaxed)) {
				case EMPTY:   armStopTrack(t); break;          // Ableton default: empty slot stops the track
				case FILLED:  sl.pending.store(LAUNCH, std::memory_order_relaxed); break;
				case PLAYING: break;                           // already the playing slot
			}
		}
	}

	// ---- Boundary commit (audio thread) ----
	void synthThumb(float* out) {
		// A plausible fake waveform so the UX can be exercised with nothing patched.
		float env = 0.f;
		for (int i = 0; i < THUMB_BINS; i++) {
			lcg = lcg * 1664525u + 1013904223u;
			float r = (float) (lcg >> 8) / 16777216.f;
			if (r > 0.93f) env = 0.5f + 0.5f * r;
			env *= 0.86f;
			out[i] = std::min(1.f, env + 0.06f * r);
		}
	}
	void setPlaying(int t, int s) {
		Track& tr = tracks[t];
		int prev = tr.playingSlot.load(std::memory_order_relaxed);
		if (prev >= 0 && prev != s)
			tr.slots[prev].state.store(FILLED, std::memory_order_relaxed);
		Slot& sl = tr.slots[s];
		sl.state.store(PLAYING, std::memory_order_relaxed);
		sl.repCount.store(0, std::memory_order_relaxed);
		sl.gain.store(1.f, std::memory_order_relaxed);
		sl.startedThisBoundary = true;
		tr.playingSlot.store(s, std::memory_order_relaxed);
	}
	void boundary() {
		for (int t = 0; t < TRACKS; t++) {
			Track& tr = tracks[t];
			// 1. Finish the rolling interval.
			for (int i = 0; i < THUMB_BINS; i++) { tr.lastLive[i] = tr.live[i]; tr.live[i] = 0.f; }
			tr.lastPeak = tr.peak;
			tr.peak = 0.f;
			const bool present = tr.present.load(std::memory_order_relaxed);
			// 2. Commit pending operations.
			for (int s = 0; s < SLOTS; s++) {
				Slot& sl = tr.slots[s];
				int p = sl.pending.exchange(NONE, std::memory_order_relaxed);
				switch (p) {
					case CAPTURE:
						if (present && tr.lastPeak < GATE) {
							sl.flashAt.store(system::getTime(), std::memory_order_relaxed); // refused
							break;
						}
						if (present) { for (int i = 0; i < THUMB_BINS; i++) sl.thumb[i] = tr.lastLive[i]; }
						else synthThumb(sl.thumb);
						sl.repeats.store(defRepeats.load(std::memory_order_relaxed), std::memory_order_relaxed);
						sl.decayDb.store(defDecayDb.load(std::memory_order_relaxed), std::memory_order_relaxed);
						setPlaying(t, s);
						break;
					case LAUNCH:
						setPlaying(t, s);
						break;
					case STOP:
						if (sl.state.load(std::memory_order_relaxed) == PLAYING) {
							sl.state.store(FILLED, std::memory_order_relaxed);
							if (tr.playingSlot.load(std::memory_order_relaxed) == s)
								tr.playingSlot.store(-1, std::memory_order_relaxed);
						}
						break;
					case OVERDUB:
						// Scaffold: the overdubbed take "looks" like old + new.
						for (int i = 0; i < THUMB_BINS; i++)
							sl.thumb[i] = std::min(1.f, std::max(sl.thumb[i],
								present ? tr.lastLive[i] : sl.thumb[i] * 0.7f + 0.2f));
						sl.repCount.store(0, std::memory_order_relaxed);
						sl.gain.store(1.f, std::memory_order_relaxed);
						sl.startedThisBoundary = true;
						break;
					default: break;
				}
			}
			// 3. The playing slot wraps: repeats + decay (not on the boundary it started on).
			int ps = tr.playingSlot.load(std::memory_order_relaxed);
			if (ps >= 0) {
				Slot& sl = tr.slots[ps];
				if (sl.startedThisBoundary) {
					sl.startedThisBoundary = false;
				} else {
					int rc = sl.repCount.load(std::memory_order_relaxed) + 1;
					sl.repCount.store(rc, std::memory_order_relaxed);
					float g = std::pow(10.f, sl.decayDb.load(std::memory_order_relaxed) * (float) rc / 20.f);
					sl.gain.store(g, std::memory_order_relaxed);
					int reps = sl.repeats.load(std::memory_order_relaxed);
					if ((reps > 0 && rc >= reps) || g < 1e-3f) {
						sl.state.store(FILLED, std::memory_order_relaxed);
						tr.playingSlot.store(-1, std::memory_order_relaxed);
					}
				}
			}
		}
	}

	void process(const ProcessArgs& args) override {
		// Simulated clock.
		intervalFrames = std::max<int64_t>(1,
			(int64_t) (SIM_SECONDS[simSecondsIdx.load(std::memory_order_relaxed)] * args.sampleRate));
		const int bin = (int) std::min<int64_t>(THUMB_BINS - 1, frameInInterval * THUMB_BINS / intervalFrames);

		// Buttons (edges on the audio thread — where the real engine will read them).
		for (int i = 0; i < TRACKS * SLOTS; i++)
			if (slotTrig[i].process(params[SLOT_PARAM + i].getValue() > 0.5f))
				pressSlot(i / SLOTS, i % SLOTS);
		for (int s = 0; s < SLOTS; s++)
			if (sceneTrig[s].process(params[SCENE_PARAM + s].getValue() > 0.5f))
				pressScene(s);
		for (int t = 0; t < TRACKS; t++)
			if (stopTrig[t].process(params[STOP_PARAM + t].getValue() > 0.5f))
				armStopTrack(t);
		if (stopAllTrig.process(params[STOP_ALL_PARAM].getValue() > 0.5f))
			for (int t = 0; t < TRACKS; t++) armStopTrack(t);

		// Inputs: LIVE strip + pass-through to MIX (the submix is real; loop playback is not).
		float mixL = 0.f, mixR = 0.f;
		const int nMulti = inputs[MULTI_INPUT].getChannels(); // 0 when unpatched
		for (int t = 0; t < TRACKS; t++) {
			Track& tr = tracks[t];
			float l = 0.f, r = 0.f;
			bool present;
			if (inputs[IN_L_INPUT + t].isConnected()) {
				// The track's own stereo jacks take precedence over the MULTI pair.
				present = true;
				l = inputs[IN_L_INPUT + t].getVoltage() * 0.2f;
				r = inputs[IN_R_INPUT + t].isConnected() ? inputs[IN_R_INPUT + t].getVoltage() * 0.2f : l;
			} else if (2 * t < nMulti) {
				// MULTI: sequential stereo pairs; an odd trailing channel is mono.
				present = true;
				l = inputs[MULTI_INPUT].getVoltage(2 * t) * 0.2f;
				r = (2 * t + 1 < nMulti) ? inputs[MULTI_INPUT].getVoltage(2 * t + 1) * 0.2f : l;
			} else {
				present = false;
			}
			tr.present.store(present, std::memory_order_relaxed);
			float a = std::max(std::fabs(l), std::fabs(r));
			if (a > tr.live[bin]) tr.live[bin] = std::min(1.f, a);
			if (a > tr.peak) tr.peak = a;
			// TX latch: a private track's live input leaves MIX (loops would still play —
			// the loop is the band's state; STOP it if you want it gone). ~10 ms fade.
			float txT = params[TX_PARAM + t].getValue() > 0.5f ? 1.f : 0.f;
			tr.txGain += (txT - tr.txGain) * 0.002f;
			mixL += l * tr.txGain; // instruments arrive already leveled + panned: a plain sum
			mixR += r * tr.txGain;
		}
		outputs[MIX_L_OUTPUT].setVoltage(clamp(mixL, -1.f, 1.f) * 5.f);
		outputs[MIX_R_OUTPUT].setVoltage(clamp(mixR, -1.f, 1.f) * 5.f);

		lights[OVERDUB_LIGHT].setBrightness(params[OVERDUB_PARAM].getValue() > 0.5f ? 1.f : 0.f);

		// Clock advance + boundary.
		frameInInterval++;
		if (frameInInterval >= intervalFrames) {
			frameInInterval = 0;
			boundary();
		}
		phase.store((float) frameInInterval / (float) intervalFrames, std::memory_order_relaxed);
		secsLeft.store((float) (intervalFrames - frameInInterval) / args.sampleRate, std::memory_order_relaxed);
	}

	// ---- UI-thread helpers ----
	void clearSlot(int t, int s) {
		Track& tr = tracks[t];
		Slot& sl = tr.slots[s];
		sl.pending.store(NONE, std::memory_order_relaxed);
		if (tr.playingSlot.load(std::memory_order_relaxed) == s)
			tr.playingSlot.store(-1, std::memory_order_relaxed);
		sl.state.store(EMPTY, std::memory_order_relaxed);
		for (int i = 0; i < THUMB_BINS; i++) sl.thumb[i] = 0.f;
	}
	int pendingCount() const {
		int n = 0;
		for (int t = 0; t < TRACKS; t++)
			for (int s = 0; s < SLOTS; s++)
				if (tracks[t].slots[s].pending.load(std::memory_order_relaxed) != NONE) n++;
		return n;
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "simSecondsIdx", json_integer(simSecondsIdx.load()));
		json_object_set_new(root, "defRepeats", json_integer(defRepeats.load()));
		json_object_set_new(root, "defDecayDb", json_real(defDecayDb.load()));
		json_t* names = json_array();
		for (int t = 0; t < TRACKS; t++)
			json_array_append_new(names, json_string(trackNames[t].c_str()));
		json_object_set_new(root, "trackNames", names);
		return root;
	}
	void dataFromJson(json_t* root) override {
		json_t* j;
		if ((j = json_object_get(root, "simSecondsIdx")))
			simSecondsIdx.store(clamp((int) json_integer_value(j), 0, N_SIM_SECONDS - 1));
		if ((j = json_object_get(root, "defRepeats"))) defRepeats.store((int) json_integer_value(j));
		if ((j = json_object_get(root, "defDecayDb"))) defDecayDb.store((float) json_number_value(j));
		if ((j = json_object_get(root, "trackNames")) && json_is_array(j)) {
			for (int t = 0; t < TRACKS && t < (int) json_array_size(j); t++) {
				const char* n = json_string_value(json_array_get(j, t));
				if (n && *n) trackNames[t] = n;
			}
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
		drawTxt(vg, FONT_BOLD, SCENE_X + SCENE_W / 2, NAME_Y + NAME_H / 2, 9.f, lpText(), "MULTI", NVG_ALIGN_CENTER);

		// ---- Controls column: Radio/Ninjam plate geometry (Theme.hpp) ----
		const float labDy = mm2px(AK_PLATE_LABEL_DY_MM);
		const NVGcolor bd = nvgRGBA(0, 0, 0, 0x55);
		drawTxt(vg, FONT_BOLD, RX, Y_DUB - 20.f, 11.f, lpText(), "OVERDUB", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, RX, Y_REPEATS - 17.f, 11.f, lpText(), "REPEATS", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, RX, Y_DECAY - 17.f, 11.f, lpText(), "DECAY", NVG_ALIGN_CENTER);
		// MIX output plate (dark; light in the dark theme): L over R, "MIX" on top, L/R
		// labels left of the jacks.
		nvgBeginPath(vg);
		nvgRoundedRect(vg, RX - MIX_PLATE_W / 2, mixPlateTop(), MIX_PLATE_W, mixPlateBottom() - mixPlateTop(), mm2px(AK_PLATE_R_MM));
		nvgFillColor(vg, akPlate());
		nvgFill(vg);
		nvgStrokeColor(vg, bd);
		nvgStrokeWidth(vg, 1.f);
		nvgStroke(vg);
		drawTxt(vg, FONT_BOLD, RX, mixPlateTop() + labDy, 11.f, akPlateText(), "MIX", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, MIX_LAB_X, mixJackY(0), 11.f, akPlateText(), "L", NVG_ALIGN_CENTER);
		drawTxt(vg, FONT_BOLD, MIX_LAB_X, mixJackY(1), 11.f, akPlateText(), "R", NVG_ALIGN_CENTER);
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
			s = string::f("SIMULATED CLOCK \xc2\xb7 %d s interval \xc2\xb7 next in %.1f s",
				SIM_SECONDS[lp->simSecondsIdx.load(std::memory_order_relaxed)],
				lp->secsLeft.load(std::memory_order_relaxed));
			int pc = lp->pendingCount();
			if (pc) s += string::f(" \xc2\xb7 %d queued", pc);
			if (sel >= 0) s += string::f(" \xc2\xb7 sel %d.%d", sel / SLOTS + 1, sel % SLOTS + 1);
		}
		drawTxt(args.vg, FONT_BOLD, box.size.x, box.size.y / 2, 9.f, lpTextDim(), s, NVG_ALIGN_RIGHT);
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
		int state = Looper::EMPTY, pending = Looper::NONE, reps = 0, repCount = 0;
		float decay = 0.f, gain = 1.f;
		bool sel = false;
		const float* thumb = nullptr;
		float preview[THUMB_BINS];
		if (lp) {
			Looper::Slot& sl = lp->tracks[t].slots[s];
			state = sl.state.load(std::memory_order_relaxed);
			pending = sl.pending.load(std::memory_order_relaxed);
			reps = sl.repeats.load(std::memory_order_relaxed);
			repCount = sl.repCount.load(std::memory_order_relaxed);
			decay = sl.decayDb.load(std::memory_order_relaxed);
			gain = sl.gain.load(std::memory_order_relaxed);
			sel = lp->selected.load(std::memory_order_relaxed) == t * SLOTS + s;
			thumb = sl.thumb;
		} else if ((t * 3 + s * 5) % 7 < 2) {
			// Library/browser preview: a few fake clips so the panel reads as a looper.
			state = (t + s) % 3 == 0 ? Looper::PLAYING : Looper::FILLED;
			for (int i = 0; i < THUMB_BINS; i++)
				preview[i] = 0.15f + 0.8f * std::fabs(std::sin(0.37f * i + t + s)) * std::exp(-0.04f * (i % 16));
			thumb = preview;
		}

		// Body.
		NVGcolor bg = state == Looper::EMPTY ? lpWell()
		            : state == Looper::PLAYING ? nvgTransRGBAf(lpGreen(), 0.22f)
		            : lpCard();
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, w, h, 3.f);
		nvgFillColor(vg, bg);
		nvgFill(vg);
		nvgStrokeColor(vg, lpBorder());
		nvgStrokeWidth(vg, 1.f);
		nvgStroke(vg);

		// Thumbnail.
		if (state != Looper::EMPTY && thumb) {
			const float bw = (w - 4.f) / THUMB_BINS;
			NVGcolor c = state == Looper::PLAYING ? lpGreen() : lpTextDim();
			nvgFillColor(vg, state == Looper::PLAYING ? nvgTransRGBAf(c, 0.35f + 0.65f * gain) : c);
			for (int i = 0; i < THUMB_BINS; i++) {
				float bh = std::max(1.f, thumb[i] * (h - 6.f));
				nvgBeginPath(vg);
				nvgRect(vg, 2.f + i * bw, h / 2 - bh / 2, std::max(0.5f, bw - 0.3f), bh);
				nvgFill(vg);
			}
		}
		// Armed for capture (or overdub): the interval being recorded fills the slot
		// left → right, like a recording clip — red for capture, amber over the take
		// for overdub. The bins come from the track's rolling recorder.
		if (lp && (pending == Looper::CAPTURE || pending == Looper::OVERDUB)) {
			Looper::Track& tr = lp->tracks[t];
			float ph = lp->phase.load(std::memory_order_relaxed);
			const float bw = (w - 4.f) / THUMB_BINS;
			int upto = (int) (ph * THUMB_BINS);
			bool present = tr.present.load(std::memory_order_relaxed);
			nvgFillColor(vg, pending == Looper::CAPTURE ? lpRed() : lpAmber());
			for (int i = 0; i <= upto && i < THUMB_BINS; i++) {
				float a = present ? tr.live[i] : 0.08f;
				float bh = std::max(1.f, a * (h - 6.f));
				nvgBeginPath(vg);
				nvgRect(vg, 2.f + i * bw, h / 2 - bh / 2, std::max(0.5f, bw - 0.3f), bh);
				nvgFill(vg);
			}
		}
		// Playhead.
		if (state == Looper::PLAYING) {
			float ph = lp ? lp->phase.load(std::memory_order_relaxed) : 0.4f;
			nvgBeginPath(vg);
			nvgRect(vg, 2.f + ph * (w - 4.f) - 0.5f, 1.f, 1.f, h - 2.f);
			nvgFillColor(vg, lpText());
			nvgFill(vg);
		}
		// Settings tag (top-right): ∞ / ×N / N left, plus ↘ when decaying.
		if (state != Looper::EMPTY) {
			std::string tag;
			if (reps == 0) tag = "\xe2\x88\x9e";
			else if (state == Looper::PLAYING) tag = string::f("%d", std::max(0, reps - repCount));
			else tag = string::f("\xc3\x97%d", reps);
			if (decay < -0.01f) tag += "\xe2\x86\x98";
			drawTxt(vg, FONT_BOLD, w - 2.5f, 5.f, 7.f, lpText(), tag, NVG_ALIGN_RIGHT);
		}
		// Armed: blinking outline colored by the queued operation.
		if (pending != Looper::NONE) {
			double tm = system::getTime();
			float a = 0.45f + 0.55f * (float) (0.5 + 0.5 * std::sin(tm * 2.0 * M_PI * 2.5));
			NVGcolor c = pending == Looper::CAPTURE ? lpRed()
			           : pending == Looper::LAUNCH ? lpGreen()
			           : pending == Looper::OVERDUB ? lpAmber()
			           : lpTextDim();
			nvgBeginPath(vg);
			nvgRoundedRect(vg, 1.f, 1.f, w - 2.f, h - 2.f, 2.5f);
			nvgStrokeColor(vg, nvgTransRGBAf(c, a));
			nvgStrokeWidth(vg, 2.f);
			nvgStroke(vg);
		}
		// Refused capture: red flash.
		if (lp) {
			double f = lp->tracks[t].slots[s].flashAt.load(std::memory_order_relaxed);
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
		Looper::Slot& sl = lp->tracks[t].slots[s];
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel(string::f("Track %d \xc2\xb7 slot %d", t + 1, s + 1)));
		menu->addChild(createMenuItem("Select (for REPEATS / DECAY)", "", [m, tt, ss]() {
			m->selected.store(tt * SLOTS + ss, std::memory_order_relaxed);
		}));
		Looper::Slot* slp = &sl;
		menu->addChild(createIndexSubmenuItem("Repeats",
			{"\xe2\x88\x9e", "1", "2", "4", "8", "16", "32", "64"},
			[slp]() { return (size_t) repeatsIndex(slp->repeats.load(std::memory_order_relaxed)); },
			[slp, m, tt, ss](size_t i) {
				slp->repeats.store(REPEAT_CHOICES[i], std::memory_order_relaxed);
				m->selected.store(tt * SLOTS + ss, std::memory_order_relaxed); // the knobs follow
			}));
		menu->addChild(createIndexSubmenuItem("Decay per repetition",
			{"0 dB (none)", "\xe2\x88\x92" "1 dB", "\xe2\x88\x92" "2 dB", "\xe2\x88\x92" "3 dB", "\xe2\x88\x92" "6 dB"},
			[slp]() { return (size_t) decayIndex(slp->decayDb.load(std::memory_order_relaxed)); },
			[slp, m, tt, ss](size_t i) {
				slp->decayDb.store(DECAY_CHOICES[i], std::memory_order_relaxed);
				m->selected.store(tt * SLOTS + ss, std::memory_order_relaxed);
			}));
		menu->addChild(createMenuItem("Clear slot", "", [m, tt, ss]() { m->clearSlot(tt, ss); },
			sl.state.load(std::memory_order_relaxed) == Looper::EMPTY));
	}
};

// Per-track TRANSMIT latch: Ninjam's TX LED (akDrawTxLed), as a param so it's
// MIDI-mappable and saved with the patch. Lit = live input on air; dark = private.
struct TxSwitch : HoverSwitch {
	void draw(const DrawArgs& args) override {
		ParamQuantity* pq = getParamQuantity();
		bool on = pq ? pq->getValue() > 0.5f : true;
		const float r = 5.5f;
		akDrawTxLed(args.vg, box.size.x / 2, box.size.y / 2, r, on, hovered);
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
			if (lp->tracks[t].slots[idx].pending.load(std::memory_order_relaxed) != Looper::NONE) return true;
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

struct LooperWidget : ModuleWidget {
	Looper* lp = nullptr;
	// Knob <-> selected-slot reconcile state (UI thread).
	int lastSel = -2;
	float lastRep = -1.f, lastDec = 99.f;

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
		addInput(createInputCentered<ThemedPJ301MPort>(Vec(SCENE_X + SCENE_W / 2, JACK_Y), module, Looper::MULTI_INPUT));
		for (int s = 0; s < SLOTS; s++) {
			GlyphButton* sc = createParam<GlyphButton>(Vec(SCENE_X, GRID_Y + s * ROW_H), module, Looper::SCENE_PARAM + s);
			sc->box.size = Vec(SCENE_W, BTN_H); sc->kind = GlyphButton::SCENE; sc->lp = module; sc->idx = s; sc->momentary = true;
			addParam(sc);
		}
		GlyphButton* sa = createParam<GlyphButton>(Vec(SCENE_X, STOP_Y), module, Looper::STOP_ALL_PARAM);
		sa->box.size = Vec(SCENE_W, stopH()); sa->kind = GlyphButton::STOP_ALL; sa->lp = module; sa->momentary = true;
		addParam(sa);

		// Bottom I/O strip.
		addParam(createParamCentered<RoundSmallBlackKnob>(Vec(RX, Y_REPEATS), module, Looper::REPEATS_PARAM));
		addParam(createParamCentered<RoundSmallBlackKnob>(Vec(RX, Y_DECAY), module, Looper::DECAY_PARAM));
		addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(MIX_JACK_X, mixJackY(0)), module, Looper::MIX_L_OUTPUT));
		addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(MIX_JACK_X, mixJackY(1)), module, Looper::MIX_R_OUTPUT));
		// Overdub: the component-library bezel button with a light (what Fundamental's
		// PUSH uses), latching; red = overdub armed.
		addParam(createLightParamCentered<VCVLightBezelLatch<RedLight>>(Vec(RX, Y_DUB), module,
			Looper::OVERDUB_PARAM, Looper::OVERDUB_LIGHT));
	}

	// REPEATS / DECAY knobs mirror the selected slot: a selection change loads the knobs;
	// a knob move writes the slot. (Same reconcile pattern as Ninjam's ParamStateSync.)
	void step() override {
		ModuleWidget::step();
		if (!lp) return;
		int sel = lp->selected.load(std::memory_order_relaxed);
		ParamQuantity* rq = lp->paramQuantities[Looper::REPEATS_PARAM];
		ParamQuantity* dq = lp->paramQuantities[Looper::DECAY_PARAM];
		if (!rq || !dq) return;
		if (sel != lastSel) {
			lastSel = sel;
			if (sel >= 0) {
				Looper::Slot& sl = lp->slotAt(sel);
				lastRep = (float) repeatsIndex(sl.repeats.load(std::memory_order_relaxed));
				lastDec = sl.decayDb.load(std::memory_order_relaxed);
				rq->setValue(lastRep);
				dq->setValue(lastDec);
			}
			return;
		}
		if (sel < 0) return;
		Looper::Slot& sl = lp->slotAt(sel);
		float r = rq->getValue();
		if (r != lastRep) {
			lastRep = r;
			sl.repeats.store(REPEAT_CHOICES[clamp((int) std::round(r), 0, N_REPEAT_CHOICES - 1)], std::memory_order_relaxed);
		}
		float d = dq->getValue();
		if (d != lastDec) {
			lastDec = d;
			sl.decayDb.store(d, std::memory_order_relaxed);
		}
		// External edits (menu) of the selected slot reflect back into the knobs.
		float curRep = (float) repeatsIndex(sl.repeats.load(std::memory_order_relaxed));
		if (curRep != lastRep) { lastRep = curRep; rq->setValue(curRep); }
		float curDec = sl.decayDb.load(std::memory_order_relaxed);
		if (curDec != lastDec) { lastDec = curDec; dq->setValue(curDec); }
	}

	void appendContextMenu(Menu* menu) override {
		Looper* m = lp;
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("UX scaffold \xe2\x80\x94 simulated clock, no audio stored"));
		menu->addChild(createIndexSubmenuItem("Simulated interval", {"2 s", "4 s", "8 s", "16 s", "32 s"},
			[m]() { return (size_t) m->simSecondsIdx.load(std::memory_order_relaxed); },
			[m](size_t i) { m->simSecondsIdx.store((int) i, std::memory_order_relaxed); }));
		menu->addChild(createIndexSubmenuItem("New clips: repeats",
			{"\xe2\x88\x9e", "1", "2", "4", "8", "16", "32", "64"},
			[m]() { return (size_t) repeatsIndex(m->defRepeats.load(std::memory_order_relaxed)); },
			[m](size_t i) { m->defRepeats.store(REPEAT_CHOICES[i], std::memory_order_relaxed); }));
		menu->addChild(createIndexSubmenuItem("New clips: decay per repetition",
			{"0 dB (none)", "\xe2\x88\x92" "1 dB", "\xe2\x88\x92" "2 dB", "\xe2\x88\x92" "3 dB", "\xe2\x88\x92" "6 dB"},
			[m]() { return (size_t) decayIndex(m->defDecayDb.load(std::memory_order_relaxed)); },
			[m](size_t i) { m->defDecayDb.store(DECAY_CHOICES[i], std::memory_order_relaxed); }));
		menu->addChild(createMenuItem("Clear all slots", "", [m]() {
			for (int t = 0; t < TRACKS; t++)
				for (int s = 0; s < SLOTS; s++)
					m->clearSlot(t, s);
			m->selected.store(-1, std::memory_order_relaxed);
		}));
	}
};

Model* modelLooper = createModel<Looper, LooperWidget>("Looper");
