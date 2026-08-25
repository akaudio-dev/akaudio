// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

// Offline test for LooperEngine + LooperWorker (docs/LOOPER_DESIGN.md §13): drives
// the engine with a synthetic interval clock and deterministic input and checks the
// state machine + buffer rotation sample-exactly. No Rack link.
//   c++ -std=c++11 -I src test/looper_engine_test.cpp src/looper/LooperEngine.cpp \
//       src/looper/LooperWorker.cpp -lpthread -o build/looper_engine_test && build/looper_engine_test
//
// Timing model: a queued action commits on the boundary, i.e. on frame 0 of the NEXT
// interval — so every state check sits after the following interval() call, whose
// whole output already reflects the committed state.
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
	int bpm = 120, bpi = 4; // the clock's tempo (BPI-conversion tests change bpi + n + gen)
	int sineHz = -1;    // input override: a pure sine (varispeed tests need a smooth signal)
	bool tx_ = false;   // TX latch fed to the track this interval
	int quietFrom = -1; // input silent from this frame on (tail-gate tests); -1 = full interval
	std::vector<float> outL, outR, cueL_, cueR_; // last interval's MIX + CUE

	// The synthetic signal is hot to the last frame, which would make every capture
	// auto-advance — the sections below predate that feature, so it's off by default
	// here and the auto-advance section re-enables it.
	Sim() { eng.autoAdvance.store(false); }

	ClockFrame clock() {
		ClockFrame c;
		c.running = true; c.intervalFrames = n; c.frameInInterval = frame; c.downbeat = frame == 0;
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
			bool live = (k >= 0 || sineHz > 0) && (quietFrom < 0 || f < quietFrom);
			float v = sineHz > 0 ? 0.3f * std::sin(2.f * (float) M_PI * sineHz * f / SR) : (k >= 0 ? sig(k, f) : 0.f);
			in.l = live ? v : 0.f;
			in.r = live ? -v : 0.f;
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
	// Output == signal a + signal b (the overdubbed take), within tol.
	bool outputIsSum(int a, int b, float tol = 1e-6f) {
		for (int f = 0; f < n; f++) {
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

	// Warm-up: first clock → regrid → rolling buffers arrive from the worker.
	sim.interval(0);
	CHECK(sim.eng.tracks[0].bufs.load() >= 2, "rolling buffers stocked (got %d)", sim.eng.tracks[0].bufs.load());

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
	// layers this interval's input onto its take each boundary, until the latch is off or
	// the selection changes. slot 0 is playing signal 1. ----
	sim.eng.overdubSel.store(0 * MAX_SLOTS + 0);   // select slot 0
	sim.eng.overdubMode.store(true);           // engage the latch
	CHECK(!s0.overdubbing.load(), "not overdubbing until the boundary arms it");
	sim.interval(2);                           // boundary arms; this interval folds signal 2
	CHECK(s0.overdubbing.load(), "selected playing cell overdubs while latched");
	CHECK(s0.state.load() == PLAYING, "overdubbing cell keeps playing");
	sim.interval(-1);                          // boundary commits: take = signal 1 + signal 2
	{
		bool ok = true;
		for (int f = 0; f < N && ok; f++) {
			float el = Sim::sig(1, f) + Sim::sig(2, f);
			float er = -Sim::sig(1, f) - Sim::sig(2, f);
			if (std::fabs(sim.outL[f] - el) > 1e-6f || std::fabs(sim.outR[f] - er) > 1e-6f) {
				std::printf("   overdub mismatch at %d: %.6f vs %.6f\n", f, sim.outL[f], el);
				ok = false;
			}
		}
		CHECK(ok, "overdub layered this interval's input onto the loop");
	}
	sim.eng.overdubMode.store(false);          // disengage → overdub stops after committing
	sim.interval(-1);                          // commits the trailing (silent) overdub, no re-arm
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
	// Pressing a recording cell cancels it.
	sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressSlot(0, 4, false); });
	sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressSlot(0, 4, false); });
	CHECK(sim.slot(4).state.load() == EMPTY, "press while recording cancels");
	sim.interval(-1);
	CHECK(sim.slot(4).state.load() == EMPTY, "cancelled recording never commits");
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

	// ---- Clear (UI intent): empties the slot AND resets its settings (an empty cell's
	// settings are visible now — a rest step — so Clear must not leave a ghost) ----
	s0.repeats.store(4); s0.decayDb.store(-3.f); s0.followSlot.store(2);
	sim.eng.requestClear(0, 0);
	sim.interval(-1);
	CHECK(s0.state.load() == EMPTY && s0.take.buf == nullptr, "clear empties the slot and returns its buffer");
	CHECK(s0.repeats.load() == 0 && s0.decayDb.load() == 0.f && s0.followSlot.load() == 0,
		"clear resets the cell's settings (no ghost rest step)");

	// ---- Regrid: a take of another length stays but isn't launchable; new captures work ----
	sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressSlot(0, 2, false); });
	sim.interval(3); // records
	sim.interval(-1);
	Slot& s2 = sim.slot(2);
	CHECK(s2.state.load() == PLAYING, "captured into slot 2 before regrid");
	CHECK(sim.outputIs(3), "slot 2 plays");
	sim.n = 2400; sim.gen = 2; sim.frame = 0;
	sim.interval(4);                          // regrid on its first tick
	CHECK(s2.state.load() == FILLED && !s2.playable.load(), "old-length take stopped + greyed on regrid");
	CHECK(sim.eng.intervalFrames.load() == 2400, "engine follows the new N");
	sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressSlot(0, 2, false); });
	sim.interval(-1);
	CHECK(s2.state.load() == FILLED && s2.flashAt.load() > 0, "launching a mismatched take is refused");
	sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressSlot(0, 3, false); });
	sim.interval(5); // records on the new grid
	sim.interval(-1);
	CHECK(sim.slot(3).state.load() == PLAYING, "capture on the new grid committed");
	CHECK(sim.outputIs(5), "capture on the new grid plays sample-exactly");
	// A follow into a grid-mismatched take refuses like a launch would (rest cells are
	// the only take-less targets allowed; wrong-length takes are not).
	sim.slot(3).repeats.store(1);
	sim.slot(3).followSlot.store(3); // → slot 2: FILLED but old-length
	s2.flashAt.store(-1.0);
	sim.interval(-1); // boundary: repeats exhaust → follow refused → stop
	CHECK(sim.slot(3).state.load() == FILLED && sim.eng.tracks[0].playingSlot.load() == -1,
		"mismatched follow target: source stops");
	CHECK(s2.flashAt.load() > 0, "mismatched follow target red-flashes");
	// An out-of-range follow value (hand-edited manifest) degrades to stop, no crash.
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
		CHECK(s6.playable.load(), "loaded take is playable (matches the live grid)");
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

	CHECK(sim.eng.tracks[0].bufs.load() <= 3, "at most 3 rolling buffers per track (got %d)", sim.eng.tracks[0].bufs.load());
	std::printf("worker allocations: %ld\n", sim.eng.allocations.load());
	CHECK(sim.eng.allocations.load() <= 20, "allocation count bounded (got %ld)", sim.eng.allocations.load());

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

	// ---- A follow whose target was chain-armed THIS boundary must not hijack the
	// recording: the finished clip stops, the chain keeps rolling. ----
	{
		Sim h;
		h.eng.autoAdvance.store(true);
		h.eng.declickEnabled.store(false);
		h.eng.start();
		h.interval(0); // warm-up
		// A clip in slot 7 to launch during the chain (quiet tail: no chain of its own).
		h.interval(-1, true, [&](int f) { if (f == 10) h.eng.pressSlot(0, 7, false); });
		h.quietFrom = h.n / 2;
		h.interval(9);
		h.quietFrom = -1;
		h.interval(-1);
		CHECK(h.slot(7).state.load() == PLAYING, "hijack: slot 7 captured");
		h.eng.stopTrack(0);
		h.interval(-1);
		h.slot(7).repeats.store(1);
		h.slot(7).followSlot.store(4); // → slot index 3: will be RECORDING when this fires
		// Chain: capture slot 0 → rolls 0→1→2→3...
		h.interval(-1, true, [&](int f) { if (f == 10) h.eng.pressSlot(0, 0, false); });
		h.interval(1); // records slot 0 (hot)
		h.interval(2, true, [&](int f) { if (f == 10) h.eng.pressSlot(0, 7, false); }); // slot 1 records; launch 7
		h.interval(3); // slot 7 plays its single pass; slot 2 records
		CHECK(h.slot(7).state.load() == PLAYING, "hijack: launched clip plays during the chain");
		h.interval(4); // boundary: slot 2 commits + arms slot 3; slot 7's follow → slot 3 must be refused
		CHECK(h.slot(3).state.load() == RECORDING, "hijack: the chain-armed cell keeps recording");
		CHECK(h.slot(7).state.load() == FILLED, "hijack: the finished clip stopped instead");
		h.interval(-1); // slot 3 commits (hot) → chain continues; then let it die out
		h.interval(-1);
		h.eng.stop();
	}

	// ---- Pickup capture (press → downbeat): what the player performs between arming a
	// capture and the downbeat folds into the take's TAIL (a loop is circular — the
	// pickup replays right before each repeat's downbeat), so early hits and lead-in
	// phrases survive. The auto-advance tail gate judges the raw take, not the fold. ----
	{
		Sim p;
		p.eng.declickEnabled.store(false);
		p.eng.start();
		p.interval(0); // warm-up
		const int armF = p.n - 600; // press 600 frames before the downbeat
		// pressSlot stamps the frame recorded by the PREVIOUS tick, so the window
		// opens one frame before the press callback's frame.
		const int from = armF - 1;
		const int fadeF = 72; // = declickN at 48 kHz (sr * 0.0015)
		p.interval(9, true, [&](int f) { if (f == armF) p.eng.pressSlot(0, 0, false); });
		p.interval(1);  // the take interval records sig 1
		p.interval(-1); // boundary commits; this interval plays the take
		CHECK(p.slot(0).state.load() == PLAYING, "pickup: take plays");
		{
			bool ok = true;
			for (int f = 0; f < p.n && ok; f++) {
				float fg = f < from ? 0.f : (f - from < fadeF ? (float) (f - from + 1) / (float) fadeF : 1.f);
				float el = Sim::sig(1, f) + Sim::sig(9, f) * fg;
				if (std::fabs(p.outL[f] - el) > 1e-5f || std::fabs(p.outR[f] + el) > 1e-5f) {
					std::printf("   pickup mismatch at %d: got %.6f want %.6f\n", f, p.outL[f], el);
					ok = false;
				}
			}
			CHECK(ok, "pickup: post-press audio in the tail, body untouched, faded-in fold");
		}
		// A folded lead-in must not read as "played through the downbeat": with
		// auto-advance on and a quiet raw tail, no chain starts.
		p.eng.autoAdvance.store(true);
		p.eng.stopTrack(0);
		p.interval(-1);
		p.interval(9, true, [&](int f) { if (f == armF) p.eng.pressSlot(0, 1, false); });
		p.quietFrom = p.n / 2;
		p.interval(2);  // take: hot first half, silent raw tail
		p.quietFrom = -1;
		p.interval(-1);
		CHECK(p.slot(1).state.load() == PLAYING, "pickup + quiet tail: loops immediately (fold doesn't trigger the chain)");
		CHECK(p.slot(2).state.load() == EMPTY, "pickup + quiet tail: nothing chained");

		// The OVERDUB latch suppresses auto-advance: playing through the downbeat
		// overdubs the committed cell instead of chaining to the next.
		p.eng.overdubMode.store(true);
		p.eng.overdubSel.store(2); // in Rack the press selects the cell; mirror that here
		p.interval(-1, true, [&](int f) { if (f == 10) p.eng.pressSlot(0, 2, true); });
		p.interval(3); // hot to the last frame — would chain without the latch
		p.interval(-1);
		CHECK(p.slot(2).state.load() == PLAYING, "overdub latch: capture plays instead of chaining");
		CHECK(p.slot(3).state.load() == EMPTY, "overdub latch: nothing chained to the next cell");
		CHECK(p.slot(2).overdubbing.load(), "overdub latch: the committed cell is the overdub target");
		p.eng.overdubMode.store(false);
		p.eng.overdubSel.store(-1);
		p.interval(-1);
		p.eng.stop();
	}

	// ---- BPI conversion: doubling tiles the take ×2; halving splits it into two
	// ×1-chained halves. Derivations are RAM-only, guarded, and never re-derived. ----
	{
		Sim b;
		b.eng.declickEnabled.store(false);
		b.eng.start();
		b.interval(0); // warm-up at bpi 4 / n 4800
		Slot& s0 = b.slot(0);
		// Capture sig 1 at bpi 4.
		b.interval(-1, true, [&](int f) { if (f == 10) b.eng.pressSlot(0, 0, false); });
		b.interval(1);
		b.interval(-1);
		CHECK(s0.state.load() == PLAYING, "bpi: base take captured");
		b.eng.stopTrack(0);
		b.interval(-1);

		// Double the BPI (4 → 8, same BPM): the take tiles to fill the new interval.
		b.bpi = 8; b.n = 9600; b.gen = 2; b.frame = 0;
		b.interval(-1); // regrid → convert → install (worker runs during the naps)
		CHECK(s0.state.load() == FILLED && s0.playable.load(), "tile: converted take is playable");
		CHECK(s0.take.frames == 9600, "tile: take now spans the doubled interval (got %d)", s0.take.frames);
		CHECK(s0.derived.load(), "tile: converted take is marked derived");
		b.interval(-1, true, [&](int f) { if (f == 10) b.eng.pressSlot(0, 0, false); });
		b.interval(-1);
		{
			bool ok = true;
			for (int f = 0; f < b.n && ok; f++) {
				float el = Sim::sig(1, f % 4800);
				if (std::fabs(b.outL[f] - el) > 1e-6f || std::fabs(b.outR[f] + el) > 1e-6f) {
					std::printf("   tile mismatch at %d: got %.6f want %.6f\n", f, b.outL[f], el);
					ok = false;
				}
			}
			CHECK(ok, "tile: playback is the take twice, sample-exact");
		}
		b.eng.stopTrack(0);
		b.interval(-1);

		// Capture sig 2 at bpi 8 into slot 2, then halve the BPI (8 → 4): the take
		// splits into two ×1-chained halves; the tile-derived slot 0 must NOT be
		// re-derived (derivations grey out on further changes).
		b.interval(-1, true, [&](int f) { if (f == 10) b.eng.pressSlot(0, 2, false); });
		b.interval(2);
		b.interval(-1);
		CHECK(b.slot(2).state.load() == PLAYING, "bpi: 8-beat take captured");
		b.eng.stopTrack(0);
		b.interval(-1);
		b.bpi = 4; b.n = 4800; b.gen = 3; b.frame = 0;
		b.interval(-1); // regrid → split installs
		CHECK(!s0.playable.load() && s0.derived.load(), "split: tile-derived cell greys, not re-derived");
		CHECK(b.slot(2).playable.load() && b.slot(2).take.frames == 4800, "split: first half fits the grid");
		CHECK(b.slot(3).state.load() == FILLED && b.slot(3).take.frames == 4800, "split: second half landed below");
		CHECK(b.slot(2).repeats.load() == 1 && b.slot(2).followSlot.load() == 4,
			"split: first half wired to the second");
		CHECK(b.slot(3).repeats.load() == 1 && b.slot(3).followSlot.load() == 3,
			"split: endless original becomes an A-B cycle");
		CHECK(b.slot(2).derived.load() && b.slot(3).derived.load(), "split: both halves marked derived");
		b.interval(-1, true, [&](int f) { if (f == 10) b.eng.pressSlot(0, 2, false); });
		b.interval(-1);
		CHECK(b.outputIs(2), "split replay: interval 1 = the first half");
		b.interval(-1);
		{
			bool ok = true;
			for (int f = 0; f < b.n && ok; f++) {
				float el = Sim::sig(2, 4800 + f);
				if (std::fabs(b.outL[f] - el) > 1e-6f || std::fabs(b.outR[f] + el) > 1e-6f) {
					std::printf("   split mismatch at %d: got %.6f want %.6f\n", f, b.outL[f], el);
					ok = false;
				}
			}
			CHECK(ok, "split replay: interval 2 = the second half, sample-exact");
		}
		b.interval(-1);
		CHECK(b.outputIs(2), "split replay: interval 3 cycles back to the first half");
		b.eng.stopTrack(0);
		b.interval(-1);

		// An occupied next slot blocks a split (the take stays grey) while its
		// neighbor with a free slot below still splits.
		b.bpi = 8; b.n = 9600; b.gen = 4; b.frame = 0;
		b.interval(-1); // regrid: everything greys (all derived or mismatched)
		b.interval(-1, true, [&](int f) { if (f == 10) b.eng.pressSlot(0, 5, false); });
		b.interval(3);
		b.interval(-1, true, [&](int f) { if (f == 10) b.eng.pressSlot(0, 6, false); });
		b.interval(4);
		b.interval(-1);
		CHECK(b.slot(5).state.load() == FILLED && b.slot(6).state.load() == PLAYING,
			"bpi: two adjacent 8-beat takes captured");
		b.eng.stopTrack(0);
		b.interval(-1);
		b.bpi = 4; b.n = 4800; b.gen = 5; b.frame = 0;
		b.interval(-1); // regrid: slot 5 blocked (6 occupied); slot 6 splits into 6+7
		CHECK(!b.slot(5).playable.load() && !b.slot(5).derived.load(),
			"split: occupied next slot leaves the take grey (and underived)");
		CHECK(b.slot(6).playable.load() && b.slot(7).state.load() == FILLED,
			"split: the neighbor with a free slot below split fine");
		b.eng.stop();
	}

	// ---- BPM conversion (varispeed): a tempo change within 0.5×–2× re-pitches takes;
	// bigger jumps and the toggle-off case grey them out instead. ----
	{
		Sim r;
		r.eng.declickEnabled.store(false);
		r.eng.start();
		r.interval(0); // warm-up (bpm 120, bpi 4, n 4800)
		Slot& s0 = r.slot(0);
		// Capture one interval of a 440 Hz sine (arm interval silent — a sine there
		// would be folded into the take's tail by pickup capture, doubling it).
		r.interval(-1, true, [&](int f) { if (f == 10) r.eng.pressSlot(0, 0, false); });
		r.sineHz = 440;
		r.interval(1); // the recorded interval (input = the sine)
		r.sineHz = -1;
		r.interval(-1);
		CHECK(s0.state.load() == PLAYING, "varispeed: sine take captured");
		r.eng.stopTrack(0);
		r.interval(-1);

		// BPM 120 → 96 (ratio 0.8): the take resamples to the longer interval and the
		// sine drops to 440·0.8 = 352 Hz.
		r.bpm = 96; r.n = 6000; r.gen = 2; r.frame = 0;
		r.interval(-1); // regrid → varispeed install
		CHECK(s0.playable.load() && s0.take.frames == 6000 && s0.derived.load(),
			"varispeed: take re-derived at the new tempo (frames %d)", s0.take.frames);
		r.interval(-1, true, [&](int f) { if (f == 10) r.eng.pressSlot(0, 0, false); });
		r.interval(-1);
		{
			bool ok = true;
			for (int f = 64; f < r.n - 64 && ok; f++) {
				double pos = (f + 0.5) * 0.8 - 0.5; // resampler's source mapping
				float el = 0.3f * (float) std::sin(2.0 * M_PI * 440.0 * pos / SR);
				if (std::fabs(r.outL[f] - el) > 0.02f) {
					std::printf("   varispeed mismatch at %d: got %.4f want %.4f\n", f, r.outL[f], el);
					ok = false;
				}
			}
			CHECK(ok, "varispeed: playback is the sine re-pitched by the tempo ratio");
		}
		r.eng.stopTrack(0);
		r.interval(-1);

		// Combined BPM + BPI change: capture at (96, 4), then (120, 8) — the take
		// resamples 1.25× faster AND tiles ×2 into the doubled interval.
		r.interval(-1, true, [&](int f) { if (f == 10) r.eng.pressSlot(0, 2, false); });
		r.sineHz = 440;
		r.interval(1);
		r.sineHz = -1;
		r.interval(-1);
		CHECK(r.slot(2).state.load() == PLAYING, "combined: take captured at 96 BPM");
		r.eng.stopTrack(0);
		r.interval(-1);
		r.bpm = 120; r.bpi = 8; r.n = 9600; r.gen = 3; r.frame = 0;
		r.interval(-1);
		CHECK(r.slot(2).playable.load() && r.slot(2).take.frames == 9600 && r.slot(2).derived.load(),
			"combined: resampled + tiled take fits the new grid");
		r.interval(-1, true, [&](int f) { if (f == 10) r.eng.pressSlot(0, 2, false); });
		r.interval(-1);
		{
			bool ok = true;
			for (int f = 64; f < 4800 - 64 && ok; f++) {
				double pos = (f + 0.5) * 1.25 - 0.5;
				float el = 0.3f * (float) std::sin(2.0 * M_PI * 440.0 * pos / SR);
				if (std::fabs(r.outL[f] - el) > 0.02f) {
					std::printf("   combined mismatch at %d: got %.4f want %.4f\n", f, r.outL[f], el);
					ok = false;
				}
				if (std::fabs(r.outL[f] - r.outL[f + 4800]) > 1e-6f) {
					std::printf("   combined tile mismatch at %d\n", f);
					ok = false;
				}
			}
			CHECK(ok, "combined: first half is the re-pitched sine, second half tiles it exactly");
		}
		r.eng.stopTrack(0);
		r.interval(-1);

		// Bounds + toggle need a REAL (underived) take — derived cells never re-derive.
		// Capture a fresh one at the current grid (120 BPM, 8 BPI).
		r.interval(-1, true, [&](int f) { if (f == 10) r.eng.pressSlot(0, 4, false); });
		r.sineHz = 440;
		r.interval(1);
		r.sineHz = -1;
		r.interval(-1);
		CHECK(r.slot(4).state.load() == PLAYING, "bounds: fresh take captured");
		r.eng.stopTrack(0);
		r.interval(-1);

		// Beyond 2× the take stays grey (still underived, so a sane tempo later works).
		r.bpm = 480; r.n = 2400; r.gen = 4; r.frame = 0;
		r.interval(-1);
		CHECK(!r.slot(4).playable.load() && !r.slot(4).derived.load(),
			"varispeed: a 4x jump is out of bounds — take stays grey");

		// Toggle off: even an in-bounds tempo change greys; re-enabling converts on
		// the next regrid (a tempo wiggle / rejoin rescans the grid).
		r.eng.repitch.store(false);
		r.bpm = 240; r.n = 4800; r.gen = 5; r.frame = 0; // ratio 2 vs the (120,8) take
		r.interval(-1);
		CHECK(!r.slot(4).playable.load(), "repitch off: tempo change greys the take");
		r.eng.repitch.store(true);
		r.gen = 6; r.frame = 0;
		r.interval(-1); // same grid, new generation → rescan converts now
		CHECK(r.slot(4).playable.load() && r.slot(4).take.frames == 4800 && r.slot(4).derived.load(),
			"repitch back on: the next regrid re-derives");
		r.eng.stop();
	}

	std::printf("%s (%d failures)\n", fails ? "FAIL" : "PASS: LooperEngine", fails);
	return fails ? 1 : 0;
}
