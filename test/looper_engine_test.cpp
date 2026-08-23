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
	bool tx_ = false; // TX latch fed to the track this interval
	std::vector<float> outL, outR, cueL_, cueR_; // last interval's MIX + CUE

	ClockFrame clock() {
		ClockFrame c;
		c.running = true; c.intervalFrames = n; c.frameInInterval = frame; c.downbeat = frame == 0;
		c.gridGeneration = gen; c.sessionFrame = session++; c.sampleRate = SR; c.bpm = 120; c.bpi = 4;
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
			in.l = k >= 0 ? sig(k, f) : 0.f;
			in.r = k >= 0 ? -sig(k, f) : 0.f;
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
	Slot& s1 = sim.slot(1);
	sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressSlot(0, 1, false); });
	sim.interval(-1); // records silence
	CHECK(s1.state.load() == RECORDING, "slot 1 recording");
	sim.interval(-1);
	CHECK(s1.state.load() == EMPTY && s1.flashAt.load() > 0, "silent recording refused with a flash");
	CHECK(s0.state.load() == PLAYING, "the playing slot was not disturbed");

	// ---- Record a new cell while another plays: the old loop plays until the new take lands ----
	sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressSlot(0, 1, false); });
	sim.interval(6); // slot 1 records 6 while slot 0 keeps playing
	CHECK(s1.state.load() == RECORDING && s0.state.load() == PLAYING, "recording slot 1 while slot 0 plays");
	CHECK(sim.outputIsSum(1, 2), "old (overdubbed) loop still audible during the recording interval");
	sim.interval(-1);
	CHECK(s1.state.load() == PLAYING && s0.state.load() == FILLED, "new take replaces the playing slot");
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

	// ---- Scene with an empty slot stops the track ----
	sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressScene(3); });
	sim.interval(-1);
	CHECK(s0.state.load() == FILLED, "scene row with empty slot stops the playing track");
	CHECK(sim.outputIs(-1), "silent after scene stop");

	// ---- Clear (UI intent) ----
	sim.eng.requestClear(0, 0);
	sim.interval(-1);
	CHECK(s0.state.load() == EMPTY && s0.take.buf == nullptr, "clear empties the slot and returns its buffer");

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
		for (int k = 0; k < THUMB_BINS; k++) li.thumb[k] = 0.5f;
		CHECK(sim.eng.submitLoad(li), "load submitted");
		sim.interval(-1);                          // tick installs it
		Slot& s6 = sim.slot(6);
		CHECK(s6.state.load() == FILLED, "loaded take installs as FILLED (state %d)", s6.state.load());
		CHECK(s6.take.buf != nullptr && s6.take.frames == n, "loaded take keeps its buffer + length");
		CHECK(s6.playable.load(), "loaded take is playable (matches the live grid)");
		CHECK(s6.thumb[0] > 0.4f, "loaded take carries a thumbnail");
		sim.interval(-1, true, [&](int f) { if (f == 10) sim.eng.pressSlot(0, 6, false); });
		sim.interval(-1);
		CHECK(s6.state.load() == PLAYING, "loaded take launches");
		CHECK(sim.outputIs(5), "loaded take plays the decoded signal sample-exactly");
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

	std::printf("%s (%d failures)\n", fails ? "FAIL" : "PASS: LooperEngine", fails);
	return fails ? 1 : 0;
}
