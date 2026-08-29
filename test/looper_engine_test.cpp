// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

// Offline test for LooperEngine + LooperWorker (docs/LOOPER_DESIGN.md §13): drives
// the engine with a synthetic interval clock and deterministic input and checks the
// state machine + buffer rotation sample-exactly. No Rack link.
//   c++ -std=c++11 -I src test/looper_engine_test.cpp src/looper/LooperEngine.cpp \
//       src/looper/LooperWorker.cpp -lpthread -o build/looper_engine_test && build/looper_engine_test
//
// Timing model: a queued action commits on the next BEAT (the fluid-jamming rework).
// The legacy sections below run the sim with bpi = 1, where the only beat IS the
// downbeat — every historical interval-quantized expectation still holds verbatim.
// The "beat action grid" section at the end runs bpi = 4 and exercises mid-interval
// launches, stops, recording starts/finishes, and free-running playback.
#include "looper/LooperEngine.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

using namespace akaudio::looper;

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// Stands in for looper::Session: records the disk jobs the engine hands the worker, and
// touches every sample of each saved take (an ASan build turns a use-after-free here into
// a hard failure — the buffer must outlive its SAVE). No OGG/file I/O, so it links with
// nothing extra.
struct MockSink : LooperSink {
	std::mutex m;
	int saves = 0, clears = 0, flushes = 0, bad = 0;
	void save(int, int, const float* pcm, const TakeMeta& meta) override {
		std::lock_guard<std::mutex> lk(m);
		saves++;
		if (!pcm || meta.frames <= 0) { bad++; return; }
		volatile double sum = 0;
		for (int i = 0; i < meta.frames * 2; i++) sum += pcm[i]; // reads the whole take
		(void) sum;
	}
	void clear(int, int) override { std::lock_guard<std::mutex> lk(m); clears++; }
	void flush() override { std::lock_guard<std::mutex> lk(m); flushes++; }
	// Serve `emptyLoads` loads with EMPTY pcm (a missing/undecodable OGG): the worker
	// must skip them — the slot stays EMPTY instead of installing a silent take.
	int emptyLoads = 0;
	bool nextLoad(int& t, int& s, std::vector<float>& pcm, int& frames, TakeMeta& meta) override {
		std::lock_guard<std::mutex> lk(m);
		if (emptyLoads <= 0) return false;
		emptyLoads--;
		t = 0; s = 5;
		pcm.clear();
		frames = 4800;
		meta = TakeMeta();
		meta.frames = 4800;
		meta.sampleRate = 48000.f;
		return true;
	}
	int nSaves() { std::lock_guard<std::mutex> lk(m); return saves; }
	int nClears() { std::lock_guard<std::mutex> lk(m); return clears; }
	int nBad() { std::lock_guard<std::mutex> lk(m); return bad; }
	// Performance events (the as-played log), in arrival order.
	std::vector<LoopEvent> events;
	void event(const LoopEvent& ev) override {
		std::lock_guard<std::mutex> lk(m);
		events.push_back(ev);
	}
	std::vector<LoopEvent> evs() { std::lock_guard<std::mutex> lk(m); return events; }
};

static const int N = 4800;       // 0.1 s @ 48k
static const float SR = 48000.f;

struct Sim {
	LooperEngine eng;
	int frame = 0;
	uint32_t gen = 1;
	uint64_t session = 0;
	double now = 0.0;
	int n = N;
	// bpi 1 = the legacy grid (beat == downbeat, interval-quantized expectations);
	// the beat-grid section runs bpi 4 (a beat every n/4 frames).
	int bpm = 600, bpi = 1;
	bool tx_ = false;   // TX latch fed to the track this interval
	int quietFrom = -1; // input silent from this frame on (tail-gate tests); -1 = full interval
	float inGain = 1.f; // input scale (finish tests: a faint bar between the −70/−40 dB gates)
	std::vector<float> outL, outR, cueL_, cueR_; // last interval's MIX + CUE

	// The synthetic signal is hot to the last frame, which would make every capture
	// auto-advance — the sections below predate that feature, so it's off by default
	// here and the auto-advance section re-enables it.
	Sim() { eng.autoAdvance.store(false); }

	ClockFrame clock() {
		ClockFrame c;
		c.running = true; c.intervalFrames = n; c.frameInInterval = frame; c.downbeat = frame == 0;
		// Beat flags exactly as JamClock computes them: index = frame*bpi/n.
		c.beatIndex = bpi > 0 ? (int) ((long long) frame * bpi / n) : 0;
		c.beat = frame == 0
			|| (bpi > 1 && c.beatIndex != (int) ((long long) (frame - 1) * bpi / n));
		c.gridGeneration = gen; c.sessionFrame = session++; c.sampleRate = SR; c.bpm = bpm; c.bpi = bpi;
		if (++frame >= n) frame = 0;
		return c;
	}
	static void nap() { std::this_thread::sleep_for(std::chrono::milliseconds(4)); }

	// Deterministic per-interval signal, |x| ≤ 0.3 so two summed stay under the
	// limiter's 0.8 knee (transparent region).
	static float sig(int k, int f) { return ((float) (((k + 1) * 7919 + f * 31) % 1000) / 1000.f - 0.5f) * 0.6f; }

	// Run one interval feeding signal `k` (or silence if k < 0). `action(f)` runs before
	// the tick of frame f. Sleeps a few ms early and mid-interval so the worker can
	// answer allocation / overdub-copy requests.
	template <typename F>
	void interval(int k, bool present, F action) {
		outL.assign(n, 0.f); outR.assign(n, 0.f); cueL_.assign(n, 0.f); cueR_.assign(n, 0.f);
		for (int f = 0; f < n; f++) {
			action(f);
			TrackIn in;
			in.present = present; in.tx = tx_;
			bool live = k >= 0 && (quietFrom < 0 || f < quietFrom);
			float v = k >= 0 ? sig(k, f) : 0.f;
			in.l = live ? v * inGain : 0.f;
			in.r = live ? -v * inGain : 0.f;
			float l = 0.f, r = 0.f, cl = 0.f, cr = 0.f;
			eng.tick(clock(), &in, 1, now += 1.0 / SR, l, r, cl, cr);
			outL[f] = l; outR[f] = r; cueL_[f] = cl; cueR_[f] = cr;
			if (f == 2 || f == n / 2) nap();
		}
	}
	void interval(int k, bool present = true) { interval(k, present, [](int) {}); }

	// Output == signal k (or silence for k < 0) times gain, within tol (0 = exact).
	bool outputIs(int k, float gain = 1.f, float tol = 0.f) {
		for (int f = 0; f < n; f++) {
			float el = k >= 0 ? sig(k, f) * gain : 0.f;
			float er = k >= 0 ? -sig(k, f) * gain : 0.f;
			if (std::fabs(outL[f] - el) > tol || std::fabs(outR[f] - er) > tol) {
				std::printf("   mismatch at frame %d: got %.6f/%.6f want %.6f/%.6f\n", f, outL[f], outR[f], el, er);
				return false;
			}
		}
		return true;
	}
	// Output == signal k truncated at `cut` (a quiet-tail take), within tol.
	bool outputIsCut(int k, int cut, float tol = 0.f) {
		for (int f = 0; f < n; f++) {
			float el = f < cut ? sig(k, f) : 0.f;
			float er = f < cut ? -sig(k, f) : 0.f;
			if (std::fabs(outL[f] - el) > tol || std::fabs(outR[f] - er) > tol) {
				std::printf("   mismatch at frame %d: got %.6f/%.6f want %.6f/%.6f\n", f, outL[f], outR[f], el, er);
				return false;
			}
		}
		return true;
	}
	// Output == signal a + signal b (the overdubbed take), within tol. Skips the first
	// 16 frames: the overdub copy arrives a hair after the wrap that re-arms it, so the
	// layer's very first frames of input are by-design absent (masked by the declick
	// fade in real use — disabled here for exactness).
	bool outputIsSum(int a, int b, float tol = 1e-6f) {
		for (int f = 16; f < n; f++) {
			float el = sig(a, f) + sig(b, f), er = -sig(a, f) - sig(b, f);
			if (std::fabs(outL[f] - el) > tol || std::fabs(outR[f] - er) > tol) {
				std::printf("   mismatch at frame %d: got %.6f want %.6f\n", f, outL[f], el);
				return false;
			}
		}
		return true;
	}
	Slot& slot(int s) { return eng.tracks[0].slots[s]; }
};

int main() {
	Sim sim;
	MockSink sink;
	sim.eng.setSink(&sink); // M4: observe the disk jobs (SAVE / CLEAR_FILE) the commits emit
	sim.eng.declickEnabled.store(false); // off for the sample-exact playback checks below
	sim.eng.start();
	Slot& s0 = sim.slot(0);

	// Warm-up: first clock → regrid → the track's spare buffer arrives from the worker.
	sim.interval(0);
	CHECK(sim.eng.tracks[0].bufs.load() >= 1, "spare buffer stocked (got %d)", sim.eng.tracks[0].bufs.load());

	// ---- Capture (Ableton clip semantics): press → records the NEXT interval → plays ----
	sim.interval(-1, true, [&](int f) { if (f == 100) sim.eng.pressSlot(0, 0, false); });
	CHECK(s0.pending.load() == CAPTURE, "capture queued");
	sim.interval(1);  // boundary: RECORDING; this interval is the take
	CHECK(s0.pending.load() == NONE && s0.state.load() == RECORDING, "recording started at the boundary (state %d)", s0.state.load());
	CHECK(sim.outputIs(-1), "nothing plays while recording the first take (tx off)");
	sim.interval(-1); // boundary: take committed, plays; silence in → out = loop
	CHECK(s0.state.load() == PLAYING, "take committed and playing (state %d)", s0.state.load());
	CHECK(sim.outputIs(1), "loop plays the recorded interval sample-exactly");
	CHECK(s0.take.startFrame == (uint64_t) (2 * N), "take stamped at its interval's start (got %llu)", (unsigned long long) s0.take.startFrame);

	// ---- Stop lands on the boundary; output continues until then ----
	sim.interval(-1, true, [&](int f) { if (f == 50) sim.eng.pressSlot(0, 0, false); });
	CHECK(s0.pending.load() == STOP && s0.state.load() == PLAYING, "stop queued, still playing");
	CHECK(sim.outputIs(1), "loop played through the interval the stop was queued in");
	sim.interval(-1);
	CHECK(s0.state.load() == FILLED, "stop committed");
	CHECK(sim.outputIs(-1), "silent after stop");

	// ---- Launch ----
	sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressSlot(0, 0, false); });
	sim.interval(-1);
	CHECK(s0.state.load() == PLAYING, "launch committed");
	CHECK(sim.outputIs(1), "launched loop plays");

	// ---- Repeats: 2 → plays two intervals then stops ----
	s0.repeats.store(2);
	sim.interval(-1);                       // wrap 1
	CHECK(s0.state.load() == PLAYING && s0.repCount.load() == 1, "repeat 1 of 2 (state %d rc %d)", s0.state.load(), s0.repCount.load());
	CHECK(sim.outputIs(1), "2nd repetition plays");
	sim.interval(-1);                       // wrap 2 → stop
	CHECK(s0.state.load() == FILLED, "stopped after 2 repeats (state %d, rc %d)", s0.state.load(), s0.repCount.load());
	CHECK(sim.outputIs(-1), "silent after the last repeat");

	// ---- Decay: −6 dB per repetition ----
	s0.repeats.store(0);
	s0.decayDb.store(-6.f);
	sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressSlot(0, 0, false); });
	sim.interval(-1);                       // first pass, gain 1
	CHECK(s0.state.load() == PLAYING, "relaunched for decay");
	CHECK(sim.outputIs(1), "decay: first pass at unity");
	sim.interval(-1);                       // wrap → gain 10^(-6/20)
	CHECK(std::fabs(s0.gain.load() - 0.501187f) < 1e-4f, "gain after one wrap = −6 dB (got %f)", s0.gain.load());
	CHECK(sim.outputIs(1, 0.501187f, 1e-5f), "decayed pass plays at −6 dB");
	s0.decayDb.store(0.f);
	sim.interval(-1);                       // wrap with decay 0 → gain back to 1
	CHECK(std::fabs(s0.gain.load() - 1.f) < 1e-6f, "gain 1 again (got %f)", s0.gain.load());

	// ---- Continuous overdub: OVERDUB latch on + selection → the selected playing cell
	// layers its input onto its take at the playhead, committing each layer at the
	// take's own wrap, until the latch is off or the selection changes. slot 0 is
	// playing signal 1. ----
	sim.eng.overdubSel.store(0 * MAX_SLOTS + 0);   // select slot 0
	sim.eng.overdubMode.store(true);           // engage the latch
	CHECK(!s0.overdubbing.load(), "not overdubbing until the engine's tick arms it");
	sim.interval(-1);                          // arms immediately; a silent layer commits at the wrap
	CHECK(s0.overdubbing.load(), "selected playing cell overdubs while latched");
	CHECK(s0.state.load() == PLAYING, "overdubbing cell keeps playing");
	sim.interval(2);                           // this cycle folds signal 2 into the layer
	sim.interval(-1);                          // the wrap committed: take = signal 1 + signal 2
	{
		bool ok = true;
		for (int f = 16; f < N && ok; f++) { // first frames: staging-arrival hole, by design
			float el = Sim::sig(1, f) + Sim::sig(2, f);
			float er = -Sim::sig(1, f) - Sim::sig(2, f);
			if (std::fabs(sim.outL[f] - el) > 1e-6f || std::fabs(sim.outR[f] - er) > 1e-6f) {
				std::printf("   overdub mismatch at %d: %.6f vs %.6f\n", f, sim.outL[f], el);
				ok = false;
			}
		}
		CHECK(ok, "overdub layered the cycle's input onto the loop");
	}
	sim.eng.overdubMode.store(false);          // disengage → the pending silent layer is dropped
	sim.interval(-1);
	CHECK(!s0.overdubbing.load(), "overdub stops when the latch is disengaged");
	CHECK(s0.state.load() == PLAYING, "loop keeps playing after overdub stops");
	sim.eng.overdubSel.store(-1);              // clear selection for the sections below

	// ---- A silent recording is refused (slot goes back to Empty) ----
	// Recording takes over the track (Ableton): the playing clip stops at the boundary
	// the capture starts on, so the old loop is never audible under the live instrument.
	Slot& s1 = sim.slot(1);
	sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressSlot(0, 1, false); });
	sim.interval(-1); // records silence
	CHECK(s1.state.load() == RECORDING, "slot 1 recording");
	CHECK(s0.state.load() == FILLED, "recording takes over: the playing clip stopped");
	CHECK(sim.outputIs(-1), "old loop not audible while recording");
	sim.interval(-1);
	CHECK(s1.state.load() == EMPTY && s1.flashAt.load() > 0, "silent recording refused with a flash");
	CHECK(s0.state.load() == FILLED, "refused capture leaves the track stopped (takeover already happened)");

	// ---- Record a new cell while another plays: recording takes over the track ----
	sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressSlot(0, 0, false); }); // relaunch 0
	sim.interval(-1);
	CHECK(s0.state.load() == PLAYING, "slot 0 relaunched after the refusal");
	sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressSlot(0, 1, false); });
	CHECK(sim.outputIsSum(1, 2), "old loop still plays through the interval the capture was queued in");
	sim.interval(6); // slot 1 records 6; slot 0 stopped on this interval's boundary
	CHECK(s1.state.load() == RECORDING && s0.state.load() == FILLED, "capture stopped the playing slot");
	CHECK(sim.outputIs(-1), "no clip audible during the recording interval (tx off)");
	sim.interval(-1);
	CHECK(s1.state.load() == PLAYING && s0.state.load() == FILLED, "new take plays; the old cell keeps its take");
	CHECK(sim.outputIs(6), "new loop plays interval 6");
	// Pressing a recording cell queues a FINISH: the take commits at the boundary and
	// plays (Ableton: clicking a recording clip takes it). Pressing again cancels the
	// queued finish and the recording rolls on; the track STOP button is the discard.
	sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressSlot(0, 4, false); });
	int pFinish = -1, pCancel = -1;
	sim.interval(7, true, [&](int f) {
		if (f == 10) { sim.eng.pressSlot(0, 4, false); pFinish = sim.slot(4).pending.load(); }
		if (f == 20) { sim.eng.pressSlot(0, 4, false); pCancel = sim.slot(4).pending.load(); }
		if (f == 30) sim.eng.pressSlot(0, 4, false); // re-queue: this one holds
	});
	CHECK(pFinish == FINISH, "press while recording queues a finish (got %d)", pFinish);
	CHECK(pCancel == NONE, "pressing again cancels the queued finish (got %d)", pCancel);
	CHECK(sim.slot(4).state.load() == RECORDING && sim.slot(4).pending.load() == FINISH,
		"the re-queued finish holds; the recording rolls on");
	sim.interval(-1); // boundary: the finish commits + plays
	CHECK(sim.slot(4).state.load() == PLAYING, "finished take committed and playing");
	CHECK(sim.outputIs(7), "finished take plays sample-exactly");
	// Back to slot 0 for the scene test.
	sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressSlot(0, 0, false); });
	sim.interval(-1);
	CHECK(s0.state.load() == PLAYING, "slot 0 relaunched");

	// ---- Arming a scene disarms other queued launches: only the latest scene fires ----
	sim.interval(-1, true, [&](int f) {
		if (f == 10) sim.eng.pressScene(1); // queues slot 1's launch (FILLED, take 6)
		if (f == 20) sim.eng.pressScene(0); // supersedes it before the boundary
	});
	CHECK(s1.pending.load() == NONE, "superseded scene's queued launch disarmed");
	sim.interval(-1);
	CHECK(s0.state.load() == PLAYING && s1.state.load() == FILLED, "only the latest scene fired");
	CHECK(sim.outputIsSum(1, 2), "row 0's clip is the one playing");

	// ---- Scene with an empty slot stops the track ----
	sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressScene(3); });
	sim.interval(-1);
	CHECK(s0.state.load() == FILLED, "scene row with empty slot stops the playing track");
	CHECK(sim.outputIs(-1), "silent after scene stop");

	// ---- A scene acting on a track disarms its armed captures (latest press wins —
	// otherwise the surviving capture's R_STEAL demote would land on the very beat of
	// the scene's launch, hard-cutting it) ----
	sim.interval(-1, true, [&](int f) {
		if (f == 10) sim.eng.pressSlot(0, 5, false); // arm a capture on empty slot 5
		if (f == 20) sim.eng.pressScene(0);          // the scene launches row 0 (FILLED)
	});
	CHECK(sim.slot(5).pending.load() == NONE, "scene on the track disarms its armed capture");
	sim.interval(-1);
	CHECK(s0.state.load() == PLAYING && sim.slot(5).state.load() == EMPTY,
		"scene launch commits; the disarmed capture never starts");
	sim.eng.stopTrack(0);
	sim.interval(-1);

	// ---- Clear (UI intent): empties the slot AND resets its settings (an empty cell's
	// settings are visible now — a rest step — so Clear must not leave a ghost) ----
	s0.repeats.store(4); s0.decayDb.store(-3.f); s0.followSlot.store(2);
	sim.eng.requestClear(0, 0);
	sim.interval(-1);
	CHECK(s0.state.load() == EMPTY && s0.take.buf == nullptr, "clear empties the slot and returns its buffer");
	CHECK(s0.repeats.load() == 0 && s0.decayDb.load() == 0.f && s0.followSlot.load() == 0,
		"clear resets the cell's settings (no ghost rest step)");

	// ---- Regrid: a tempo change NEVER touches committed audio — the playing take
	// keeps looping at its own recorded period, free-running against the new grid;
	// old-length takes stay launchable; new captures use the new grid. ----
	sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressSlot(0, 2, false); });
	sim.interval(3); // records
	sim.interval(-1);
	Slot& s2 = sim.slot(2);
	CHECK(s2.state.load() == PLAYING, "captured into slot 2 before regrid");
	CHECK(sim.outputIs(3), "slot 2 plays");
	sim.n = 2400; sim.gen = 2; sim.frame = 0;
	sim.interval(-1);                         // regrid on its first tick; the take plays on
	CHECK(s2.state.load() == PLAYING, "free-run: the old-length take keeps playing across a regrid");
	CHECK(sim.eng.intervalFrames.load() == 2400, "engine follows the new N");
	{
		bool ok = true;
		for (int f = 0; f < sim.n && ok; f++)
			if (std::fabs(sim.outL[f] - Sim::sig(3, f)) > 1e-6f || std::fabs(sim.outR[f] + Sim::sig(3, f)) > 1e-6f) ok = false;
		CHECK(ok, "free-run: the take's first half fills the first short interval");
	}
	sim.interval(-1);
	{
		bool ok = true;
		for (int f = 0; f < sim.n && ok; f++)
			if (std::fabs(sim.outL[f] - Sim::sig(3, 2400 + f)) > 1e-6f || std::fabs(sim.outR[f] + Sim::sig(3, 2400 + f)) > 1e-6f) ok = false;
		CHECK(ok, "free-run: its second half fills the next (drifting against the grid)");
	}
	sim.eng.stopTrack(0);
	sim.interval(-1);
	CHECK(s2.state.load() == FILLED, "free-running take stopped");
	// Relaunching the old-length take works — length vs the live grid is irrelevant.
	sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressSlot(0, 2, false); });
	sim.interval(-1);
	CHECK(s2.state.load() == PLAYING, "old-length take relaunches after the regrid");
	sim.eng.stopTrack(0);
	sim.interval(-1);
	// New captures record on the new grid.
	sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressSlot(0, 3, false); });
	sim.interval(5); // records on the new grid
	sim.interval(-1);
	CHECK(sim.slot(3).state.load() == PLAYING, "capture on the new grid committed");
	CHECK(sim.outputIs(5), "capture on the new grid plays sample-exactly");
	// A follow into an old-length take is allowed too — it free-runs from the jump.
	sim.slot(3).repeats.store(1);
	sim.slot(3).followSlot.store(3); // → slot 2: FILLED, old length
	sim.interval(-1); // wrap: repeats exhaust → follow into the mismatched take
	CHECK(s2.state.load() == PLAYING && sim.eng.tracks[0].playingSlot.load() == 2,
		"follow into an old-length take launches it");
	sim.eng.stopTrack(0);
	sim.interval(-1);
	// An out-of-range follow value (hand-edited manifest) degrades to stop, no crash.
	sim.slot(3).repeats.store(1);
	sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressSlot(0, 3, false); });
	sim.interval(-1);
	CHECK(sim.slot(3).state.load() == PLAYING, "relaunched for the out-of-range follow test");
	sim.slot(3).followSlot.store(99); // repeats still 1
	sim.interval(-1);
	CHECK(sim.slot(3).state.load() == FILLED && sim.eng.tracks[0].playingSlot.load() == -1,
		"out-of-range follow value degrades to stop");
	sim.slot(3).repeats.store(0);
	sim.slot(3).followSlot.store(0);

	// ---- CUE bus: a private (TX-off) track's live-thru goes to CUE, not MIX ----
	sim.eng.requestClear(0, 3);
	sim.interval(-1);
	sim.tx_ = false; // track 0 private
	sim.interval(7); // live input, no loop, tx off -> should appear on CUE, not MIX
	{
		bool mixSilent = true, cueHas = false;
		for (int f = 0; f < sim.n; f++) {
			if (std::fabs(sim.outL[f]) > 1e-4f) mixSilent = false;
			if (std::fabs(sim.cueL_[f]) > 1e-3f) cueHas = true;
		}
		CHECK(mixSilent, "private track's live input is NOT in MIX");
		CHECK(cueHas, "private track's live input IS in CUE");
	}
	sim.tx_ = true;

	// ---- M4: commits reached the disk sink with valid, fully-readable take buffers ----
	CHECK(sink.nSaves() >= 5, "captures + overdub were handed to the sink (got %d)", sink.nSaves());
	CHECK(sink.nBad() == 0, "every SAVE carried a non-null take buffer of its full length");
	CHECK(sink.nClears() >= 1, "cleared slots were retired via the sink (got %d)", sink.nClears());

	// ---- Clip loader: a decoded take (submitLoad) installs as a FILLED slot and launches ----
	{
		const int n = sim.n;                       // the current grid length
		Buf* b = new Buf; b->frames = n; b->pcm = new float[(size_t) n * 2];
		for (int f = 0; f < n; f++) { b->pcm[f * 2] = Sim::sig(5, f); b->pcm[f * 2 + 1] = -Sim::sig(5, f); }
		LoadInstall li{};
		li.track = 0; li.slot = 6; li.buf = b;
		li.meta.frames = n; li.meta.sampleRate = SR; li.meta.bpm = 120; li.meta.bpi = 4;
		li.meta.repeats = 0; li.meta.decayDb = 0.f; li.meta.startFrame = 0; li.meta.peak = 0.3f;
		li.meta.followSlot = 3;
		for (int k = 0; k < THUMB_BINS; k++) li.thumb[k] = 0.5f;
		CHECK(sim.eng.submitLoad(li), "load submitted");
		sim.interval(-1);                          // tick installs it
		Slot& s6 = sim.slot(6);
		CHECK(s6.state.load() == FILLED, "loaded take installs as FILLED (state %d)", s6.state.load());
		CHECK(s6.followSlot.load() == 3, "loaded take restores its follow action");
		CHECK(s6.take.buf != nullptr && s6.take.frames == n, "loaded take keeps its buffer + length");
		CHECK(s6.thumb[0] > 0.4f, "loaded take carries a thumbnail");
		sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressSlot(0, 6, false); });
		sim.interval(-1);
		CHECK(s6.state.load() == PLAYING, "loaded take launches");
		CHECK(sim.outputIs(5), "loaded take plays the decoded signal sample-exactly");
	}

	// ---- A load with empty pcm (missing/undecodable OGG) is skipped, never installed ----
	{
		{ std::lock_guard<std::mutex> lk(sink.m); sink.emptyLoads = 1; }
		sim.interval(-1);
		sim.interval(-1);
		CHECK(sim.slot(5).state.load() == EMPTY, "empty-pcm load skipped (no silent ghost take)");
	}

	// ---- A load landing on a PLAYING slot tears it down first (regression 2026-08-29:
	// it forced FILLED while playingSlot still pointed at it — audibly playing yet
	// unstoppable, since STOP gates on state == PLAYING) ----
	{
		const int n = sim.n;
		CHECK(sim.slot(6).state.load() == PLAYING, "clobber setup: slot 6 still playing");
		Buf* b = new Buf; b->frames = n; b->pcm = new float[(size_t) n * 2];
		for (int f = 0; f < n; f++) { b->pcm[f * 2] = Sim::sig(8, f); b->pcm[f * 2 + 1] = -Sim::sig(8, f); }
		LoadInstall li{};
		li.track = 0; li.slot = 6; li.buf = b;
		li.meta.frames = n; li.meta.sampleRate = SR; li.meta.bpm = 120; li.meta.bpi = 4;
		CHECK(sim.eng.submitLoad(li), "clobber load submitted");
		sim.interval(-1); // install lands at the first tick: playing span closed
		CHECK(sim.slot(6).state.load() == FILLED
			&& sim.eng.tracks[0].playingSlot.load() == -1,
			"load onto a playing slot stops it cleanly (FILLED, playingSlot cleared)");
		sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressSlot(0, 6, false); });
		sim.interval(-1);
		CHECK(sim.slot(6).state.load() == PLAYING, "clobbered slot relaunches");
		CHECK(sim.outputIs(8), "the newly loaded take plays sample-exactly");
		sim.eng.stopTrack(0);
		sim.interval(-1);
	}

	CHECK(sim.eng.tracks[0].bufs.load() <= 1, "at most one spare buffer per track (got %d)", sim.eng.tracks[0].bufs.load());
	std::printf("worker allocations: %ld\n", sim.eng.allocations.load());
	CHECK(sim.eng.allocations.load() <= 40, "allocation count bounded (got %ld)", sim.eng.allocations.load());

	sim.eng.stop();

	// ---- Declick: a playing loop fades each cycle's ends to 0, so the loop point is
	// click-free, while the body plays sample-exactly ----
	{
		Sim d; // fresh engine: declick on by default
		d.eng.start();
		d.interval(0);                                             // warm-up (rolling buffers)
		d.interval(-1, true, [&](int f) { if (f == 5) d.eng.pressSlot(0, 0, false); }); // arm capture
		d.interval(2);                                             // records signal 2 as the take
		d.interval(-1);                                            // committed + playing; outL = the loop
		CHECK(d.slot(0).state.load() == PLAYING, "declick: loop is playing");
		CHECK(std::fabs(d.outL[0]) < 0.02f, "declick: loop starts near zero (got %.4f)", d.outL[0]);
		CHECK(std::fabs(d.outL[d.n - 1]) < 0.02f, "declick: loop ends near zero (got %.4f)", d.outL[d.n - 1]);
		float mid = Sim::sig(2, d.n / 2);
		CHECK(std::fabs(d.outL[d.n / 2] - mid) < 1e-4f, "declick: loop body is unattenuated (got %.4f want %.4f)", d.outL[d.n / 2], mid);
		// The fade is short: well before the midpoint the loop is already at full level.
		int q = d.n / 8;
		CHECK(std::fabs(d.outL[q] - Sim::sig(2, q)) < 1e-4f, "declick: fade is short (full level by n/8)");
		d.eng.stop();
	}

	// ---- Follow actions: when a clip is done (repeats exhausted / decayed below the
	// floor) its "After" setting runs — 0 = stop (the old behavior), 1..8 = launch that
	// slot on the same track (self = retrigger at full gain). One hop per boundary. ----
	{
		Sim w;
		w.eng.declickEnabled.store(false);
		w.eng.start();
		Slot& a = w.slot(0);
		Slot& b = w.slot(1);
		Slot& c = w.slot(2);
		w.interval(0); // warm-up (rolling buffers)

		// Fill slot 0 = signal 1, slot 1 = signal 2, slot 2 = signal 3. A stale follow
		// set before a capture must not survive the commit (fresh takes default to Stop).
		a.followSlot.store(5);
		w.interval(-1, true, [&](int f) { if (f == 10) w.eng.pressSlot(0, 0, false); });
		w.interval(1);
		w.interval(-1);
		CHECK(a.state.load() == PLAYING, "follow: slot 0 captured");
		CHECK(a.followSlot.load() == 0, "capture resets the follow action to Stop");
		w.interval(-1, true, [&](int f) { if (f == 10) w.eng.pressSlot(0, 1, false); });
		w.interval(2);
		w.interval(-1);
		CHECK(b.state.load() == PLAYING, "follow: slot 1 captured");
		w.interval(-1, true, [&](int f) { if (f == 10) w.eng.pressSlot(0, 2, false); });
		w.interval(3);
		w.interval(-1);
		CHECK(c.state.load() == PLAYING, "follow: slot 2 captured");
		w.eng.stopTrack(0);
		w.interval(-1);
		CHECK(w.eng.tracks[0].playingSlot.load() == -1, "follow: track stopped for the scenarios");

		// Exhausted repeats jump to the target, which then behaves exactly like a launch:
		// with repeats 1 it plays one interval, not two (the wrap isn't skipped).
		a.repeats.store(2); a.followSlot.store(2);   // after 2 passes → slot 2 (index 1)
		b.repeats.store(1); b.followSlot.store(0);   // the target then stops after 1 pass
		w.interval(-1, true, [&](int f) { if (f == 10) w.eng.pressSlot(0, 0, false); });
		w.interval(-1);
		CHECK(w.outputIs(1), "follow: pass 1 of 2 plays");
		w.interval(-1);
		CHECK(w.outputIs(1), "follow: pass 2 of 2 plays");
		w.interval(-1); // boundary: repeats exhausted → jump
		CHECK(w.outputIs(2), "follow: target plays the very next interval");
		CHECK(a.state.load() == FILLED && b.state.load() == PLAYING, "follow: source demoted, target playing");
		CHECK(w.eng.tracks[0].playingSlot.load() == 1, "follow: playingSlot moved to the target");
		CHECK(std::fabs(b.gain.load() - 1.f) < 1e-6f, "follow: target starts at full gain");
		w.interval(-1); // the jumped-to clip's own repeats=1 → exactly one interval
		CHECK(w.outputIs(-1), "follow: jumped-to clip plays exactly one interval (like a launch)");
		CHECK(b.state.load() == FILLED, "follow: chain ended by the target's Stop");

		// Follow → self: retrigger — replays every interval at full gain, decay never
		// accumulates (repCount/gain reset each cycle).
		b.repeats.store(1); b.decayDb.store(-6.f); b.followSlot.store(2); // self (slot index 1)
		w.interval(-1, true, [&](int f) { if (f == 10) w.eng.pressSlot(0, 1, false); });
		for (int pass = 0; pass < 3; pass++) {
			w.interval(-1);
			CHECK(w.outputIs(2), "retrigger: pass %d replays at unity", pass + 1);
			CHECK(b.state.load() == PLAYING, "retrigger: still playing (pass %d)", pass + 1);
			CHECK(std::fabs(b.gain.load() - 1.f) < 1e-6f, "retrigger: gain reset (pass %d)", pass + 1);
		}
		w.eng.stopTrack(0);
		w.interval(-1);
		b.decayDb.store(0.f); b.followSlot.store(0);
		CHECK(b.state.load() == FILLED, "retrigger: explicit stop ends the loop");

		// Decaying out (gain under the −60 dB floor) fires the follow action too.
		a.repeats.store(0); a.decayDb.store(-40.f); a.followSlot.store(2);
		w.interval(-1, true, [&](int f) { if (f == 10) w.eng.pressSlot(0, 0, false); });
		w.interval(-1);
		CHECK(w.outputIs(1), "decay-follow: first pass at unity");
		w.interval(-1);
		CHECK(w.outputIs(1, 0.01f, 1e-5f), "decay-follow: second pass at −40 dB");
		w.interval(-1); // gain would be 1e-4 < 1e-3 → follow fires
		CHECK(w.outputIs(2), "decay-follow: decayed-out clip chains to the target");
		CHECK(b.state.load() == PLAYING, "decay-follow: target playing");
		w.interval(-1); // b: repeats 1, follow 0 → done
		a.decayDb.store(0.f);

		// A follow into an EMPTY cell is a "rest": it plays silence for its own repeat
		// count, then runs its own follow — here a one-interval gap inside an A-rest-A
		// cycle. (A grid-mismatched target still refuses — same guard as LAUNCH.)
		a.repeats.store(1); a.followSlot.store(6); // slot index 5: EMPTY = rest
		w.slot(5).repeats.store(1);
		w.slot(5).followSlot.store(1);             // rest → back to slot 0
		w.interval(-1, true, [&](int f) { if (f == 10) w.eng.pressSlot(0, 0, false); });
		w.interval(-1);
		CHECK(w.outputIs(1), "rest: the clip's single pass plays");
		w.interval(-1);
		CHECK(w.outputIs(-1), "rest: one interval of silence");
		CHECK(w.slot(5).state.load() == PLAYING, "rest: the empty cell is 'playing' its silence");
		CHECK(w.eng.tracks[0].playingSlot.load() == 5, "rest: playingSlot points at the rest cell");
		w.interval(-1);
		CHECK(w.outputIs(1), "rest: the cycle comes back around to the clip");
		w.eng.stopTrack(0);
		w.interval(-1);
		CHECK(w.slot(5).state.load() == EMPTY, "rest: a stopped rest cell goes back to EMPTY, not FILLED");
		w.slot(5).repeats.store(0); w.slot(5).followSlot.store(0);

		// An explicit launch committed at the same boundary wins over the follow jump.
		a.repeats.store(1); a.followSlot.store(2); // would chain to slot 1
		w.interval(-1, true, [&](int f) { if (f == 10) w.eng.pressSlot(0, 0, false); });
		w.interval(-1, true, [&](int f) { if (f == 10) w.eng.pressSlot(0, 2, false); }); // final pass; user launches slot 2
		CHECK(w.outputIs(1), "precedence: the final pass plays through");
		w.interval(-1);
		CHECK(w.outputIs(3), "precedence: the user's launch plays, not the follow target");
		CHECK(c.state.load() == PLAYING && b.state.load() == FILLED, "precedence: follow target untouched");
		w.eng.stopTrack(0);
		w.interval(-1);

		// Chain A→B→C→stop: one clip per interval, sample-exact.
		a.repeats.store(1); a.followSlot.store(2);
		b.repeats.store(1); b.followSlot.store(3);
		c.repeats.store(1); c.followSlot.store(0);
		w.interval(-1, true, [&](int f) { if (f == 10) w.eng.pressSlot(0, 0, false); });
		w.interval(-1);
		CHECK(w.outputIs(1), "chain: interval 1 = A");
		w.interval(-1);
		CHECK(w.outputIs(2), "chain: interval 2 = B");
		w.interval(-1);
		CHECK(w.outputIs(3), "chain: interval 3 = C");
		w.interval(-1);
		CHECK(w.outputIs(-1), "chain: ends with C's Stop");
		CHECK(a.state.load() == FILLED && b.state.load() == FILLED && c.state.load() == FILLED,
			"chain: every clip back to FILLED");
		w.eng.stop();
	}

	// ---- Auto-advance capture (tail gate): playing through the downbeat rolls the
	// recording into the next empty cell; stopping before it loops the take at once.
	// A fully-silent interval (the existing gate) ends the chain with nothing playing. ----
	{
		Sim v;
		v.eng.autoAdvance.store(true); // the feature under test (on by default in the plugin)
		v.eng.declickEnabled.store(false);
		v.eng.start();
		v.interval(0); // warm-up

		// Quiet tail (player stopped before the loop point): no chain — the take plays
		// immediately, exactly the pre-feature behavior.
		v.interval(-1, true, [&](int f) { if (f == 10) v.eng.pressSlot(0, 0, false); });
		v.quietFrom = v.n / 2;
		v.interval(1);  // records sig 1, second half silent (tail window = n/2)
		v.quietFrom = -1;
		v.interval(-1); // boundary commits
		CHECK(v.slot(0).state.load() == PLAYING, "tail-gate: quiet tail loops immediately");
		CHECK(v.outputIsCut(1, v.n / 2), "tail-gate: the truncated take plays sample-exactly");
		v.eng.stopTrack(0);
		v.interval(-1);

		// Hot tail: the capture chains down the column, one interval per cell, silent
		// while it rolls (recording still owns the track).
		v.interval(-1, true, [&](int f) { if (f == 10) v.eng.pressSlot(0, 1, false); });
		v.interval(2);  // records sig 2, hot to the last frame
		v.interval(3);  // boundary: slot 1 commits → chains; this interval records sig 3
		CHECK(v.slot(1).state.load() == FILLED, "auto-advance: committed cell stays FILLED (not playing)");
		CHECK(v.slot(2).state.load() == RECORDING, "auto-advance: next empty cell records the continuation");
		CHECK(v.eng.tracks[0].playingSlot.load() == -1, "auto-advance: nothing plays while the chain rolls");
		CHECK(v.outputIs(-1), "auto-advance: silent during the chain");
		v.interval(-1); // boundary: slot 2 commits (hot) → chains on; records silence
		CHECK(v.slot(2).state.load() == FILLED && v.slot(3).state.load() == RECORDING,
			"auto-advance: chain advances again");
		v.interval(-1); // boundary: the silent interval is refused → chain over
		CHECK(v.slot(3).state.load() == EMPTY && v.slot(3).flashAt.load() > 0,
			"auto-advance: silence ends the chain (refused, flashed)");
		CHECK(v.eng.tracks[0].playingSlot.load() == -1, "auto-advance: chain end leaves the track stopped");
		CHECK(v.outputIs(-1), "auto-advance: silent after the chain ends");

		// The chain wired itself: each cell one pass, follow → the next, last one stops.
		CHECK(v.slot(1).repeats.load() == 1 && v.slot(1).followSlot.load() == 3,
			"auto-advance: first chained cell wired (x1, follow -> next)");
		CHECK(v.slot(2).repeats.load() == 1 && v.slot(2).followSlot.load() == 0,
			"auto-advance: last chained cell wired (x1, follow Stop)");
		// Launching the first cell replays the whole performance in order, once.
		v.interval(-1, true, [&](int f) { if (f == 10) v.eng.pressSlot(0, 1, false); });
		v.interval(-1);
		CHECK(v.outputIs(2), "auto-advance replay: interval 1 = first cell");
		v.interval(-1);
		CHECK(v.outputIs(3), "auto-advance replay: interval 2 follows to the second cell");
		v.interval(-1);
		CHECK(v.outputIs(-1), "auto-advance replay: performance ends with silence");
		CHECK(v.slot(1).state.load() == FILLED && v.slot(2).state.load() == FILLED,
			"auto-advance replay: cells back to FILLED");

		// Bottom of the column: nowhere to chain → the take plays despite the hot tail.
		v.interval(-1, true, [&](int f) { if (f == 10) v.eng.pressSlot(0, 7, false); });
		v.interval(4);
		v.interval(-1);
		CHECK(v.slot(7).state.load() == PLAYING, "auto-advance: bottom cell plays (no next slot)");
		CHECK(v.outputIs(4), "auto-advance: bottom take plays sample-exactly");

		// Occupied next slot: the chain never overwrites a take → the capture plays.
		v.eng.requestClear(0, 0);
		v.interval(-1);
		v.interval(-1, true, [&](int f) { if (f == 10) v.eng.pressSlot(0, 0, false); });
		v.interval(5);
		v.interval(-1);
		CHECK(v.slot(0).state.load() == PLAYING, "auto-advance: occupied next slot → no chain, take plays");
		CHECK(v.outputIs(5), "auto-advance: that take plays sample-exactly");

		// The toggle off restores plain capture even with a hot tail (slot 4 is empty,
		// so only the toggle prevents the chain).
		v.eng.autoAdvance.store(false);
		v.interval(-1, true, [&](int f) { if (f == 10) v.eng.pressSlot(0, 3, false); });
		v.interval(6);
		v.interval(-1);
		CHECK(v.slot(3).state.load() == PLAYING, "auto-advance off: hot tail still loops immediately");
		CHECK(v.outputIs(6), "auto-advance off: plays sample-exactly");
		v.eng.autoAdvance.store(true);

		// A quiet tail ends a chain mid-column: the outro cell commits silent (FILLED,
		// one pass) instead of looping alone, and the replay covers the whole take.
		v.interval(-1, true, [&](int f) { if (f == 10) v.eng.pressSlot(0, 4, false); });
		v.interval(7);                 // records sig 7, hot → will chain
		v.quietFrom = v.n / 2;
		v.interval(8);                 // slot 4 commits + chains; slot 5 records the outro
		v.quietFrom = -1;
		v.interval(-1);                // boundary: outro commits with a quiet tail → chain over
		CHECK(v.slot(5).state.load() == FILLED, "quiet-tail end: outro cell commits silent (no lone loop)");
		CHECK(v.eng.tracks[0].playingSlot.load() == -1, "quiet-tail end: nothing auto-plays");
		CHECK(v.slot(4).repeats.load() == 1 && v.slot(4).followSlot.load() == 6,
			"quiet-tail end: predecessor wired to the outro");
		CHECK(v.slot(5).repeats.load() == 1 && v.slot(5).followSlot.load() == 0,
			"quiet-tail end: outro plays once, then stops");
		v.interval(-1, true, [&](int f) { if (f == 10) v.eng.pressSlot(0, 4, false); });
		v.interval(-1);
		CHECK(v.outputIs(7), "quiet-tail replay: interval 1 = the hot cell");
		v.interval(-1);
		CHECK(v.outputIsCut(8, v.n / 2), "quiet-tail replay: interval 2 = the outro, truncated tail");
		v.interval(-1);
		CHECK(v.outputIs(-1), "quiet-tail replay: then silence");

		// Clearing a chain's ARMED cell mid-roll ends the chain cleanly: the predecessor
		// is stamped as the last cell, and a later capture into the cleared cell is a
		// normal standalone capture (not misclassified as chained).
		v.eng.requestClear(0, 2);
		v.eng.requestClear(0, 3);
		v.interval(-1);
		v.interval(-1, true, [&](int f) { if (f == 10) v.eng.pressSlot(0, 2, false); });
		v.interval(1); // records hot → will chain into slot 3
		v.interval(2, true, [&](int f) { if (f == 100) v.eng.requestClear(0, 3); }); // clear the armed cell mid-interval
		CHECK(v.slot(3).state.load() == EMPTY, "clear-armed: armed cell emptied");
		CHECK(v.slot(2).repeats.load() == 1, "clear-armed: predecessor stamped as the last cell");
		// A fresh capture into the cleared cell is standalone (quiet tail → plays).
		v.interval(-1, true, [&](int f) { if (f == 10) v.eng.pressSlot(0, 3, false); });
		v.quietFrom = v.n / 2;
		v.interval(3);
		v.quietFrom = -1;
		v.interval(-1);
		CHECK(v.slot(3).state.load() == PLAYING, "clear-armed: later capture is standalone (plays, not parked)");
		CHECK(v.slot(2).followSlot.load() == 0, "clear-armed: predecessor was never rewired to the cleared cell");
		v.eng.stopTrack(0);
		v.interval(-1);

		// Clearing the chain's PREDECESSOR mid-roll: the armed cell commits standalone,
		// and no ghost settings are written back into the cleared cell.
		v.eng.requestClear(0, 2);
		v.eng.requestClear(0, 3);
		v.interval(-1);
		v.interval(-1, true, [&](int f) { if (f == 10) v.eng.pressSlot(0, 2, false); });
		v.interval(1); // records hot → chains into slot 3
		v.quietFrom = v.n / 2;
		v.interval(4, true, [&](int f) { if (f == 100) v.eng.requestClear(0, 2); }); // clear the predecessor
		v.quietFrom = -1;
		v.interval(-1); // boundary: slot 3 commits — standalone now (quiet tail → plays)
		CHECK(v.slot(3).state.load() == PLAYING, "clear-predecessor: successor commits standalone and plays");
		CHECK(v.slot(2).state.load() == EMPTY && v.slot(2).followSlot.load() == 0
			&& v.slot(2).repeats.load() == 0, "clear-predecessor: no ghost settings in the cleared cell");
		v.eng.stop();
	}

	// ---- Finish press: clicking the recording cell commits the bar at the boundary
	// and replays — an auto-advance chain closes and cycles from its head; a final bar
	// below the −40 dB tail gate is dropped (no dead bar in the loop); a hot tail does
	// NOT chain (the press means "this bar is the last"). ----
	{
		Sim p;
		p.eng.autoAdvance.store(true);
		p.eng.declickEnabled.store(false);
		p.eng.start();
		p.interval(0); // warm-up

		// Unchained finish with a hot tail: no chain — the take loops at once.
		p.interval(-1, true, [&](int f) { if (f == 10) p.eng.pressSlot(0, 0, false); });
		p.interval(1, true, [&](int f) { if (f == 100) p.eng.pressSlot(0, 0, false); }); // hot to the end
		CHECK(p.slot(0).state.load() == RECORDING && p.slot(0).pending.load() == FINISH,
			"finish: queued on the recording cell");
		p.interval(-1);
		CHECK(p.slot(0).state.load() == PLAYING, "finish: hot tail did NOT chain — take plays");
		CHECK(p.slot(1).state.load() == EMPTY, "finish: no cell armed below");
		CHECK(p.outputIs(1), "finish: the finished take plays sample-exactly");
		p.eng.stopTrack(0);
		p.interval(-1);
		p.eng.requestClear(0, 0);
		p.interval(-1);

		// Chained finish, audible final bar: the chain closes at the pressed cell,
		// which is wired back to the head — the whole performance cycles, starting at
		// the very same boundary the final bar commits on.
		p.interval(-1, true, [&](int f) { if (f == 10) p.eng.pressSlot(0, 0, false); });
		p.interval(1); // records sig 1, hot → will chain
		p.interval(2); // slot 0 commits + chains; slot 1 records sig 2
		p.interval(3, true, [&](int f) { if (f == 100) p.eng.pressSlot(0, 2, false); }); // slot 1 commits + chains; slot 2 records sig 3; finish pressed on it
		CHECK(p.slot(2).state.load() == RECORDING && p.slot(2).pending.load() == FINISH,
			"chain finish: queued on the chain's recording cell");
		p.interval(-1); // boundary: final bar commits, head launches
		CHECK(p.slot(2).state.load() == FILLED, "chain finish: final bar committed");
		CHECK(p.slot(0).state.load() == PLAYING, "chain finish: head replays at the same boundary");
		CHECK(p.outputIs(1), "chain finish: replay interval 1 = head");
		CHECK(p.slot(0).repeats.load() == 1 && p.slot(0).followSlot.load() == 2,
			"chain finish: head wired x1 -> next");
		CHECK(p.slot(1).repeats.load() == 1 && p.slot(1).followSlot.load() == 3,
			"chain finish: middle wired x1 -> next");
		CHECK(p.slot(2).repeats.load() == 1 && p.slot(2).followSlot.load() == 1,
			"chain finish: final bar follows back to the head");
		p.interval(-1);
		CHECK(p.outputIs(2), "chain finish: replay follows to the middle");
		p.interval(-1);
		CHECK(p.outputIs(3), "chain finish: replay reaches the final bar");
		p.interval(-1);
		CHECK(p.outputIs(1), "chain finish: the performance cycles back to the head");
		p.eng.stopTrack(0);
		p.interval(-1);

		// Chained finish, faint final bar (between the −70 dB silence gate and the
		// −40 dB tail gate — a decay tail, not playing): the bar is dropped, the chain
		// closes at the previous cell, and the performance still cycles from the head.
		p.eng.requestClearAll();
		p.interval(-1);
		p.interval(-1, true, [&](int f) { if (f == 10) p.eng.pressSlot(0, 0, false); });
		p.interval(4); // records sig 4, hot → will chain
		p.interval(5); // slot 0 commits + chains; slot 1 records sig 5
		p.inGain = 0.01f; // peak ~0.003: above −70 dB, below the −40 dB tail gate
		p.interval(6, true, [&](int f) { if (f == 100) p.eng.pressSlot(0, 2, false); });
		p.inGain = 1.f;
		p.interval(-1); // boundary: the faint bar is dropped; head launches
		CHECK(p.slot(2).state.load() == EMPTY, "faint finish: dead final bar dropped");
		CHECK(p.slot(2).flashAt.load() < 0, "faint finish: the drop is not an error (no flash)");
		CHECK(p.slot(1).repeats.load() == 1 && p.slot(1).followSlot.load() == 1,
			"faint finish: chain closed at the previous cell, wired back to the head");
		CHECK(p.slot(0).state.load() == PLAYING, "faint finish: head replays");
		CHECK(p.outputIs(4), "faint finish: replay interval 1 = head");
		p.interval(-1);
		CHECK(p.outputIs(5), "faint finish: replay follows to the last kept bar");
		p.interval(-1);
		CHECK(p.outputIs(4), "faint finish: the two-bar performance cycles");
		p.eng.stopTrack(0);
		p.interval(-1);

		// Press-again cancels the queued finish: the chain keeps rolling.
		p.eng.requestClearAll();
		p.interval(-1);
		p.interval(-1, true, [&](int f) { if (f == 10) p.eng.pressSlot(0, 0, false); });
		p.interval(7); // records sig 7, hot → will chain
		p.interval(8, true, [&](int f) {
			if (f == 100) p.eng.pressSlot(0, 1, false); // queue finish on the chained recording
			if (f == 200) p.eng.pressSlot(0, 1, false); // press again = cancel it
		});
		p.interval(9); // boundary: no finish → the hot tail chains on as usual
		CHECK(p.slot(1).state.load() == FILLED && p.slot(2).state.load() == RECORDING
			&& p.eng.tracks[0].playingSlot.load() == -1,
			"finish cancel: chain keeps rolling, nothing plays");
		p.eng.stopTrack(0);
		p.interval(-1);

		// The track STOP button remains the discard: it wins over a queued finish.
		p.interval(-1, true, [&](int f) { if (f == 10) p.eng.pressSlot(0, 3, false); });
		p.interval(1, true, [&](int f) {
			if (f == 100) p.eng.pressSlot(0, 3, false); // queue finish
			if (f == 200) p.eng.stopTrack(0);           // discard wins, immediately
		});
		CHECK(p.slot(3).state.load() == EMPTY && p.slot(3).pending.load() == NONE,
			"finish vs stop: discarded at once, finish disarmed");
		p.interval(-1);
		CHECK(p.slot(3).state.load() == EMPTY && p.eng.tracks[0].playingSlot.load() == -1,
			"finish vs stop: nothing commits or plays");
		p.eng.stop();
	}

	// ---- Any press disarms the rolling recording (chain included); committed chain
	// cells keep their takes. (The old follow-vs-chain-armed hijack scenario is no
	// longer constructible: one-queued-action-per-track means a capture and a launch
	// can't be co-armed — the :597 refusal guard remains as pure defense.) ----
	{
		Sim h;
		h.eng.autoAdvance.store(true);
		h.eng.declickEnabled.store(false);
		h.eng.start();
		h.interval(0); // warmup
		// Capture slot 0 with a hot-to-the-end signal: the chain rolls 0 → 1 → 2…
		h.interval(-1, true, [&](int f) { if (f == 10) h.eng.pressSlot(0, 0, false); });
		h.interval(1); // records slot 0
		h.interval(2); // commits 0 (hot tail) → arms slot 1
		h.interval(3); // commits 1 → arms slot 2
		CHECK(h.slot(0).state.load() == FILLED && h.slot(1).state.load() == FILLED
			&& h.slot(2).state.load() == RECORDING, "chain: rolling down the column");
		// Press the FILLED slot 0 mid-interval: the launch queues AND the chain's
		// armed cell disarms immediately; committed cells stay.
		// (This interval's boundary committed slot 2 and armed slot 3.)
		bool cancelled = false;
		h.interval(4, true, [&](int f) {
			if (f == 100) h.eng.pressSlot(0, 0, false);
			if (f == 200) cancelled = h.slot(3).state.load() == EMPTY
			                          && h.slot(2).state.load() == FILLED;
		});
		CHECK(cancelled, "press during chain disarms the rolling recording at once");
		CHECK(h.slot(0).pending.load() == LAUNCH || h.slot(0).state.load() == PLAYING,
			"the pressed cell is armed to launch");
		h.interval(-1); // the queued launch commits
		CHECK(h.slot(0).state.load() == PLAYING, "the pressed clip launched at the boundary");
		CHECK(h.slot(1).state.load() == FILLED && h.slot(1).take.buf,
			"already-committed chain cells keep their takes");
		h.eng.stop();
	}

	// ---- Performance events: the as-played log (docs §12) ----
	{
		Sim e;
		MockSink es;
		e.eng.setSink(&es);
		e.eng.start();
		auto last = [&](int back = 0) {
			auto v = es.evs();
			return v[v.size() - 1 - (size_t) back];
		};
		e.interval(-1); // warmup: buffer allocation
		CHECK(es.evs().empty(), "events: nothing before any commit");

		// Capture: press → records the next interval → commits + starts playing.
		e.interval(1, true, [&](int f) { if (f == 100) e.eng.pressSlot(0, 0, false); });
		e.interval(1);
		uint64_t sfCommit = e.session; // boundary sf of the next interval() call
		e.interval(-1);
		Sim::nap();
		{
			auto v = es.evs();
			CHECK(v.size() == 1, "events: capture commit emitted one event (got %zu)", v.size());
			const LoopEvent& ev = last();
			CHECK(ev.start && ev.track == 0 && ev.slot == 0 && ev.reason == LoopEvent::R_CAPTURE,
				"events: capture START on (0,0), reason capture");
			CHECK(ev.sessionFrame == sfCommit, "events: START stamped at the commit boundary (got %llu want %llu)",
				(unsigned long long) ev.sessionFrame, (unsigned long long) sfCommit);
			CHECK(ev.takeStartFrame == sfCommit - (uint64_t) e.n,
				"events: take identity = the recorded interval's start");
			CHECK(!ev.rest && ev.bpm == e.bpm && ev.bpi == e.bpi, "events: grid stamped");
		}

		// Capture steal: recording another slot stops the playing one at the arm boundary.
		e.interval(1, true, [&](int f) { if (f == 10) e.eng.pressSlot(0, 1, false); });
		uint64_t sfSteal = e.session;
		e.interval(2);
		uint64_t sfCommit1 = e.session;
		e.interval(-1);
		Sim::nap();
		{
			auto v = es.evs();
			CHECK(v.size() == 3, "events: steal STOP + commit START (got %zu)", v.size());
			CHECK(!last(1).start && last(1).slot == 0 && last(1).reason == LoopEvent::R_STEAL
				&& last(1).sessionFrame == sfSteal, "events: capture stole the track at the arm boundary");
			CHECK(last().start && last().slot == 1 && last().reason == LoopEvent::R_CAPTURE
				&& last().sessionFrame == sfCommit1, "events: new take's START at its commit");
		}

		// Launch replaces: slot 0 launches, slot 1 stops — same boundary frame.
		uint64_t sfLaunch = e.session + (uint64_t) e.n; // press this interval, commits next boundary
		e.interval(-1, true, [&](int f) { if (f == 10) e.eng.pressSlot(0, 0, false); });
		e.interval(-1);
		Sim::nap();
		{
			auto v = es.evs();
			CHECK(v.size() == 5, "events: launch = STOP + START (got %zu)", v.size());
			CHECK(!last(1).start && last(1).slot == 1 && last(1).reason == LoopEvent::R_REPLACED,
				"events: playing slot replaced");
			CHECK(last().start && last().slot == 0 && last().reason == LoopEvent::R_LAUNCH,
				"events: launch START");
			CHECK(last().sessionFrame == last(1).sessionFrame && last().sessionFrame == sfLaunch,
				"events: replace pair shares the boundary frame");
			CHECK(last().takeStartFrame == sfCommit - (uint64_t) e.n,
				"events: relaunch carries the original take identity");
		}

		// Stop button.
		e.interval(-1, true, [&](int f) { if (f == 10) e.eng.pressSlot(0, 0, false); });
		e.interval(-1);
		Sim::nap();
		CHECK(!last().start && last().slot == 0 && last().reason == LoopEvent::R_STOP,
			"events: STOP commit logged");

		// Follow to a rest cell, then the rest exhausts: START(rest) then STOP(exhausted).
		e.slot(0).repeats.store(1);
		e.slot(0).followSlot.store(3); // slot index 2 — EMPTY: a rest step
		e.slot(2).repeats.store(1);
		e.interval(-1, true, [&](int f) { if (f == 10) e.eng.pressSlot(0, 0, false); });
		e.interval(-1); // launch commits; plays its 1 repeat
		e.interval(-1); // exhausted → follow to the rest cell
		e.interval(-1); // rest exhausts → stop
		Sim::nap();
		{
			auto v = es.evs();
			bool sawRestStart = false, sawRestStop = false;
			for (const LoopEvent& ev : v) {
				if (ev.slot == 2 && ev.start && ev.rest && ev.reason == LoopEvent::R_FOLLOW)
					sawRestStart = true;
				if (ev.slot == 2 && !ev.start && ev.rest && ev.reason == LoopEvent::R_EXHAUSTED)
					sawRestStop = true;
			}
			CHECK(sawRestStart, "events: follow into a rest cell logged (rest START)");
			CHECK(sawRestStop, "events: rest exhaustion logged (rest STOP)");
		}
		e.slot(0).repeats.store(0);
		e.slot(0).followSlot.store(0);
		e.slot(2).repeats.store(0);

		// Self-retrigger (follow → itself): the span continues, no events.
		e.slot(0).repeats.store(1);
		e.slot(0).followSlot.store(1); // itself
		e.interval(-1, true, [&](int f) { if (f == 10) e.eng.pressSlot(0, 0, false); });
		e.interval(-1);
		Sim::nap();
		size_t nBeforeRetrig = es.evs().size();
		e.interval(-1); // exhausts + follows to itself
		e.interval(-1);
		Sim::nap();
		CHECK(es.evs().size() == nBeforeRetrig, "events: self-retrigger emits nothing (span continues)");
		e.slot(0).repeats.store(0);
		e.slot(0).followSlot.store(0);

		// Clear the playing cell mid-interval: STOP stamped with the current frame.
		size_t nBeforeClear = es.evs().size();
		e.interval(-1, true, [&](int f) { if (f == 137) e.eng.requestClear(0, 0); });
		Sim::nap();
		{
			auto v = es.evs();
			CHECK(v.size() == nBeforeClear + 1, "events: clear logged one STOP (got %zu, had %zu)",
				v.size(), nBeforeClear);
			CHECK(!last().start && last().reason == LoopEvent::R_CLEAR
				&& last().sessionFrame % (uint64_t) e.n != 0,
				"events: clear STOP stamped mid-interval (sf %llu)", (unsigned long long) last().sessionFrame);
		}

		// Session adoption: CARRY_SPANS re-opens the playing cells' spans in the new
		// events log (files/rows travel via Session::migrateTo, not the engine — no
		// re-encodes are enqueued).
		e.interval(-1, true, [&](int f) { if (f == 10) e.eng.pressSlot(0, 1, false); });
		e.interval(-1); // slot 1 (the earlier capture) launches
		Sim::nap();
		int savesBefore = es.nSaves();
		size_t evBefore = es.evs().size();
		e.eng.requestCarrySpans();
		e.interval(-1);
		Sim::nap();
		CHECK(es.nSaves() == savesBefore, "carry: no re-encodes enqueued (%d)", es.nSaves());
		{
			auto v = es.evs();
			CHECK(v.size() == evBefore + 1 && v.back().start && v.back().slot == 1
				&& v.back().reason == LoopEvent::R_CARRY,
				"carry: the playing span re-opens with reason carry");
		}
		// CLEAR_ALL: the whole grid as one intent (a 64-cell burst of singles would
		// overflow the 63-slot queue and silently keep one cell).
		e.interval(-1, true, [&](int f) { if (f == 10) e.eng.pressSlot(0, 1, false); });
		e.interval(-1); // slot 1 playing again; slot 0 was cleared earlier
		int clearsBefore = es.nClears();
		e.eng.requestClearAll();
		e.interval(-1);
		Sim::nap();
		bool allEmpty = true;
		for (int sl2 = 0; sl2 < 8; sl2++)
			if (e.slot(sl2).state.load() != EMPTY) allEmpty = false;
		CHECK(allEmpty, "clear-all: every cell on the track is EMPTY");
		CHECK(es.nClears() > clearsBefore, "clear-all: file retirements reached the sink");
		e.eng.stop();
	}

	// ---- One queued action per instrument: the latest press wins ----
	{
		Sim q;
		MockSink qs;
		q.eng.setSink(&qs);
		q.eng.start();
		q.interval(-1); // warmup
		// Two takes: capture slot 0, then slot 1 (the capture steals; both end FILLED).
		q.interval(1, true, [&](int f) { if (f == 10) q.eng.pressSlot(0, 0, false); });
		q.interval(1);
		q.interval(2, true, [&](int f) { if (f == 10) q.eng.pressSlot(0, 1, false); });
		q.interval(2);
		q.interval(-1);
		q.eng.stopTrack(0);
		q.interval(-1);
		CHECK(q.slot(0).state.load() == FILLED && q.slot(1).state.load() == FILLED,
			"latest-wins: two takes parked");
		// Queue launches on BOTH cells in one interval: the later press disarms the
		// earlier cell's pending, and only the later cell plays at the boundary.
		q.interval(-1, true, [&](int f) {
			if (f == 10) q.eng.pressSlot(0, 1, false); // would win by slot order anyway…
			if (f == 20) q.eng.pressSlot(0, 0, false); // …so press the LOWER slot last
		});
		CHECK(q.slot(1).pending.load() == NONE && q.slot(0).pending.load() == LAUNCH,
			"latest-wins: only the last-pressed cell stays armed");
		q.interval(-1);
		CHECK(q.slot(0).state.load() == PLAYING && q.slot(1).state.load() == FILLED,
			"latest-wins: the last-pressed cell plays, not the highest slot");
		q.eng.stop();
	}

	// ---- The BEAT action grid (fluid jamming): launch, stop, record-start and finish
	// all commit on the next beat — mid-interval is the point. bpi = 4 → a beat every
	// n/4 = 1200 frames; takes free-run from wherever they started. ----
	{
		Sim g;
		g.bpm = 2400; g.bpi = 4; // 4 beats per 0.1 s interval
		const int B = N / 4;      // 1200 frames per beat
		g.eng.declickEnabled.store(false);
		g.eng.start();
		g.interval(0); // warm-up

		// A full-interval take, classically aligned: press late in the prior interval —
		// the next beat is the downbeat itself.
		g.interval(-1, true, [&](int f) {
			if (f == g.n - 100) g.eng.pressSlot(0, 0, false);
			if (f == g.n - 50) Sim::nap(); // the staging ALLOC lands before the beat
		});
		g.interval(1);   // records the whole interval
		g.interval(-1);  // capped + playing from the downbeat
		CHECK(g.slot(0).state.load() == PLAYING, "beat grid: full-interval take captured");
		CHECK(g.outputIs(1), "beat grid: plays sample-exactly");
		CHECK(g.slot(0).take.frames == g.n, "beat grid: take length = the interval cap");
		g.eng.stopTrack(0);
		g.interval(-1);
		CHECK(g.slot(0).state.load() == FILLED, "beat grid: stopped");

		// LAUNCH commits mid-interval, on the next beat, from the take's own start.
		g.interval(-1, true, [&](int f) { if (f == 100) g.eng.pressSlot(0, 0, false); });
		{
			bool ok = true;
			for (int f = 0; f < g.n && ok; f++) {
				float want = f < B ? 0.f : Sim::sig(1, f - B);
				if (std::fabs(g.outL[f] - want) > 1e-6f || std::fabs(g.outR[f] + want) > 1e-6f) {
					std::printf("   beat-launch mismatch at %d: %.6f vs %.6f\n", f, g.outL[f], want);
					ok = false;
				}
			}
			CHECK(ok, "beat launch: silence to the beat, then the take from its own start");
		}
		CHECK(g.slot(0).state.load() == PLAYING, "beat launch: playing");

		// STOP commits on a beat too; before it lands, the free playhead wraps at the
		// take's OWN period (mid-interval, since the launch was off the downbeat).
		g.interval(-1, true, [&](int f) { if (f == 1300) g.eng.pressSlot(0, 0, false); });
		{
			bool ok = true;
			for (int f = 0; f < g.n && ok; f++) {
				float want;
				if (f < B)          want = Sim::sig(1, 3600 + f); // cycle 1 tail
				else if (f < 2 * B) want = Sim::sig(1, f - B);    // wrapped at its own period
				else                want = 0.f;                   // stopped on the next beat
				if (std::fabs(g.outL[f] - want) > 1e-6f || std::fabs(g.outR[f] + want) > 1e-6f) {
					std::printf("   beat-stop mismatch at %d: %.6f vs %.6f\n", f, g.outL[f], want);
					ok = false;
				}
			}
			CHECK(ok, "beat stop: plays (and wraps) to the stop beat, then silence");
		}
		CHECK(g.slot(0).state.load() == FILLED, "beat stop: cell FILLED");

		// Recording starts MID-INTERVAL, on the beat after the press; the take spans
		// the interval boundary, caps at one interval, and replays seamlessly — with
		// the sub-beat press→beat pre-roll folded at its tail (the pickup).
		g.eng.requestClear(0, 0);
		g.interval(-1);
		uint64_t sess2 = g.session; // session frame of the next interval's frame 0
		g.interval(2, true, [&](int f) {
			if (f == 100) g.eng.pressSlot(0, 1, false);
			if (f == 104) Sim::nap(); // staging lands well before the start beat
		});
		CHECK(g.slot(1).state.load() == RECORDING, "beat record: started mid-interval");
		g.interval(3);   // caps at f=B → commits + plays
		CHECK(g.slot(1).state.load() == PLAYING, "beat record: capped at one interval, playing");
		CHECK(g.slot(1).take.startFrame == sess2 + (uint64_t) B,
			"beat record: startFrame stamped at the mid-interval start beat (got %llu want %llu)",
			(unsigned long long) g.slot(1).take.startFrame, (unsigned long long) (sess2 + (uint64_t) B));
		{
			bool ok = true;
			for (int f = 0; f < g.n && ok; f++) {
				// take[p] = sig2(B+p) for the body, so the replay from the cap beat
				// reproduces the performance in place: out[f] = sig2(f).
				float want = f < B ? 0.f : Sim::sig(2, f);
				if (std::fabs(g.outL[f] - want) > 1e-6f || std::fabs(g.outR[f] + want) > 1e-6f) {
					std::printf("   beat-record mismatch at %d: %.6f vs %.6f\n", f, g.outL[f], want);
					ok = false;
				}
			}
			CHECK(ok, "beat record: the take replays its own start right at the cap");
		}
		g.interval(-1);
		{
			// Cycle 1's tail carries the folded pickup — take[3600+f] = sig3(f)+sig2(f):
			// the lead-in replays right before the loop point, exactly as performed.
			// Then the wrap (mid-interval) starts cycle 2's body. The first ~200 frames
			// are skipped: the pre-roll begins at staging arrival, a few frames after
			// the press.
			bool ok = true;
			for (int f = 200; f < g.n && ok; f++) {
				float want = f < B ? Sim::sig(3, f) + Sim::sig(2, f) : Sim::sig(2, f);
				if (std::fabs(g.outL[f] - want) > 1e-6f || std::fabs(g.outR[f] + want) > 1e-6f) {
					std::printf("   pickup mismatch at %d: %.6f vs %.6f\n", f, g.outL[f], want);
					ok = false;
				}
			}
			CHECK(ok, "beat record: boundary-spanning take + pickup tail replay sample-exactly");
		}

		// FINISH mid-take: the take is any whole-beat length — record one beat, commit
		// at the next; the one-beat loop cycles from its commit beat.
		g.eng.stopTrack(0);
		g.interval(-1);
		g.interval(4, true, [&](int f) {
			if (f == 10)   g.eng.pressSlot(0, 2, false); // capture: starts at f=B
			if (f == 14)   Sim::nap();
			if (f == 1400) g.eng.pressSlot(0, 2, false); // finish: commits at f=2B
		});
		CHECK(g.slot(2).state.load() == PLAYING, "beat finish: one-beat take committed and playing");
		CHECK(g.slot(2).take.frames == B, "beat finish: whole-beat take length (got %d)", g.slot(2).take.frames);
		{
			bool ok = true;
			for (int f = 0; f < g.n && ok; f++) {
				float want = f < 2 * B ? 0.f : Sim::sig(4, B + ((f - 2 * B) % B));
				if (std::fabs(g.outL[f] - want) > 1e-6f || std::fabs(g.outR[f] + want) > 1e-6f) {
					std::printf("   beat-finish mismatch at %d: %.6f vs %.6f\n", f, g.outL[f], want);
					ok = false;
				}
			}
			CHECK(ok, "beat finish: the one-beat loop cycles from its commit beat");
		}

		// Repeats count the take's OWN cycles — a ×2 one-beat loop stops mid-interval.
		g.eng.stopTrack(0);
		g.interval(-1);
		g.slot(2).repeats.store(2);
		g.interval(-1, true, [&](int f) { if (f == 10) g.eng.pressSlot(0, 2, false); });
		{
			bool ok = true;
			for (int f = 0; f < g.n && ok; f++) {
				float want = (f >= B && f < 3 * B) ? Sim::sig(4, B + ((f - B) % B)) : 0.f;
				if (std::fabs(g.outL[f] - want) > 1e-6f || std::fabs(g.outR[f] + want) > 1e-6f) {
					std::printf("   free-run repeats mismatch at %d: %.6f vs %.6f\n", f, g.outL[f], want);
					ok = false;
				}
			}
			CHECK(ok, "free-run repeats: two OWN cycles then stop, all mid-interval");
		}
		CHECK(g.slot(2).state.load() == FILLED, "free-run repeats: cell FILLED after its 2 cycles");

		// Overdubbing a finish-shortened take must NOT erase it (regression 2026-08-29:
		// the staging kept Buf::frames == N while take.frames == len, the worker's copy
		// guard failed, and a silent overdub cycle zeroed the take). Latch a silent
		// overdub across several of its own cycles and check the audio survives.
		g.slot(2).repeats.store(0);
		g.interval(-1, true, [&](int f) { if (f == 10) g.eng.pressSlot(0, 2, false); }); // launch at B
		g.eng.overdubSel.store(0 * MAX_SLOTS + 2);
		g.eng.overdubMode.store(true);
		g.interval(-1); // arm + several silent layer cycles commit (3 wraps per interval)
		g.eng.overdubMode.store(false);
		g.eng.overdubSel.store(-1);
		g.interval(-1); // trailing partial commits; the loop keeps playing
		{
			bool ok = true;
			for (int f = 0; f < g.n && ok; f++) {
				float want = Sim::sig(4, B + (f % B)); // phase 0 at frame 0: launch beat + 3 cycles
				if (std::fabs(g.outL[f] - want) > 1e-6f || std::fabs(g.outR[f] + want) > 1e-6f) {
					std::printf("   short-take overdub mismatch at %d: %.6f vs %.6f\n", f, g.outL[f], want);
					ok = false;
				}
			}
			CHECK(ok, "silent overdub cycles leave a finish-shortened take intact");
		}

		// Dropping the latch mid-cycle keeps the partial layer (regression 2026-08-29:
		// the retarget path discarded the staging, losing the phrase played since the
		// last wrap).
		g.eng.overdubSel.store(0 * MAX_SLOTS + 2);
		g.eng.overdubMode.store(true);
		g.interval(-1); // arm on a cycle boundary; silent cycles commit
		g.interval(5, true, [&](int f) { if (f == B / 2) g.eng.overdubMode.store(false); });
		g.eng.overdubSel.store(-1);
		g.interval(-1);
		{
			bool ok = true;
			for (int f = 0; f < g.n && ok; f++) {
				int p = f % B;
				if (p < 16 || (p >= B / 2 - 16 && p < B / 2 + 16)) continue; // arm/commit-edge slack
				float want = Sim::sig(4, B + p) + (p < B / 2 ? Sim::sig(5, p) : 0.f);
				if (std::fabs(g.outL[f] - want) > 1e-6f || std::fabs(g.outR[f] + want) > 1e-6f) {
					std::printf("   partial-layer mismatch at %d: %.6f vs %.6f\n", f, g.outL[f], want);
					ok = false;
				}
			}
			CHECK(ok, "mid-cycle latch drop commits the partial layer instead of discarding it");
		}
		g.eng.stop();
	}

	std::printf("%s (%d failures)\n", fails ? "FAIL" : "PASS: LooperEngine", fails);
	return fails ? 1 : 0;
}
