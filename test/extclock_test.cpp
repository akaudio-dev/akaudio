// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov
//
// Offline test for the Looper's standalone external clock (src/looper/ExtClock.hpp,
// docs/LOOPER_DESIGN.md §3.5). No Rack link — ExtClock is header-only and Rack-free.
//
// Build:
//   c++ -std=c++11 -I src test/extclock_test.cpp -o build/extclock_test && build/extclock_test
#include "looper/ExtClock.hpp"
#include <cstdio>
#include <cmath>

using akaudio::looper::ExtClock;
using akaudio::looper::ClockFrame;

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } } while (0)

static const float SR = 48000.f;

// --- PHASE: a ramp that wraps every `Np` frames should give one downbeat + `bpi` beats per
// cycle, frameInInterval == position in the cycle, and the latched N == Np. ---
static void testPhase() {
	ExtClock ec;
	ec.onActivate(4, SR);
	const int Np = 400, bpi = 4;
	int downbeats = 0, beats = 0;
	bool everRunning = false;
	for (int k = 0; k < 3 * Np; k++) {
		float p = (float) (k % Np) / (float) Np;   // 0 .. ~1, wraps at k%Np==0
		ClockFrame c{};
		bool ext = ec.tick(true, p * 10.f, false, 0.f, false, 0.f, bpi, SR, c);
		CHECK(ext, "phase: external owns the clock when PHASE is patched");
		if (!c.running) continue;
		everRunning = true;
		// Third cycle: fully warmed, N latched exactly.
		if (k >= 2 * Np) {
			int pos = k % Np;
			CHECK(c.intervalFrames == Np, "phase: latched N == cycle length");
			CHECK(c.frameInInterval == pos, "phase: frameInInterval tracks the ramp");
			CHECK(c.beatIndex == pos * bpi / Np, "phase: beatIndex subdivides the interval");
			CHECK(c.downbeat == (pos == 0), "phase: downbeat only at the wrap");
			CHECK(c.bpi == bpi, "phase: bpi passed through");
			if (c.downbeat) downbeats++;
			if (c.beat) beats++;
		}
	}
	CHECK(everRunning, "phase: produced a running grid");
	CHECK(downbeats == 1, "phase: exactly one downbeat in the measured cycle");
	CHECK(beats == bpi, "phase: exactly bpi beats in the measured cycle");
}

// --- PHASE freeze: a stationary ramp for > 0.25 s halts (running=false), and motion
// resumes it. ---
static void testPhaseFreeze() {
	ExtClock ec;
	ec.onActivate(4, SR);
	const int Np = 400;
	// Run a couple of cycles so it's locked.
	for (int k = 0; k < 2 * Np; k++) {
		ClockFrame c{};
		ec.tick(true, (float) (k % Np) / Np * 10.f, false, 0.f, false, 0.f, 4, SR, c);
	}
	// Hold phase constant well past the 0.25 s threshold (12000 frames @ 48k).
	ClockFrame c{};
	bool runningLate = true;
	for (int k = 0; k < 15000; k++)
		ec.tick(true, 5.f, false, 0.f, false, 0.f, 4, SR, c);
	runningLate = c.running;
	CHECK(!runningLate, "phase freeze: a stationary ramp halts the clock");
	CHECK(ec.stopped, "phase freeze: stopped flag set");
	// Resume motion → runs again.
	bool resumed = false;
	float p = 0.5f;
	for (int k = 0; k < 50; k++) {
		p += 0.002f; if (p > 1.f) p -= 1.f;
		ClockFrame d{};
		ec.tick(true, p * 10.f, false, 0.f, false, 0.f, 4, SR, d);
		if (d.running) resumed = true;
	}
	CHECK(resumed, "phase freeze: motion resumes the clock");
}

// --- CLOCK + RESET, no RESET patched: CLOCK edges are beats; the interval is bpi beats;
// N == bpi * measured beat length; the downbeat is every bpi-th edge. ---
static void testPulseNoReset() {
	ExtClock ec;
	ec.onActivate(4, SR);
	const int B = 500, bpi = 4;
	int downbeats = 0;
	int lastDownbeatK = -1, measuredPeriod = -1;
	for (int k = 0; k <= 12 * B; k++) {
		float clk = (k % B == 0) ? 5.f : 0.f; // one-frame pulse every B frames
		ClockFrame c{};
		bool ext = ec.tick(false, 0.f, true, clk, false, 0.f, bpi, SR, c);
		CHECK(ext, "pulse: external owns the clock when CLOCK is patched");
		if (c.running) {
			CHECK(c.intervalFrames == bpi * B, "pulse: N == bpi * beat length");
			if (c.downbeat) {
				if (lastDownbeatK >= 0) measuredPeriod = k - lastDownbeatK;
				lastDownbeatK = k;
				downbeats++;
				CHECK(c.frameInInterval == 0, "pulse: frameInInterval resets on the downbeat");
			}
		}
	}
	CHECK(downbeats >= 2, "pulse: downbeats fire");
	CHECK(measuredPeriod == bpi * B, "pulse: downbeat period == one interval");
}

// --- CLOCK + RESET patched: RESET defines the interval length (its period), independent of
// bpi. ---
static void testPulseWithReset() {
	ExtClock ec;
	ec.onActivate(4, SR);
	const int B = 500, R = 1500; // reset every 3 beats — N must follow RESET, not bpi
	int downbeats = 0, lastDbK = -1, period = -1;
	for (int k = 0; k <= 10 * R; k++) {
		float clk = (k % B == 0) ? 5.f : 0.f;
		float rst = (k % R == 0) ? 5.f : 0.f;
		ClockFrame c{};
		ec.tick(false, 0.f, true, clk, true, rst, 4, SR, c);
		if (c.running && c.downbeat) {
			if (lastDbK >= 0) period = k - lastDbK;
			lastDbK = k; downbeats++;
			CHECK(c.intervalFrames == R, "pulse+reset: N == RESET period (not bpi*beat)");
		}
	}
	CHECK(downbeats >= 2, "pulse+reset: downbeats fire on RESET");
	CHECK(period == R, "pulse+reset: downbeat period == RESET period");
}

// --- N stability: a jittery measured period (±1 frame, as integer counting always gives)
// must NOT move the reported intervalFrames — else the engine would full-regrid every
// interval and discard every in-flight recording. Only a real tempo change moves N. ---
static void testNStability() {
	ExtClock ec;
	ec.onActivate(4, SR);
	const int bpi = 4;
	int reportedN = -1, distinctN = 0;
	int64_t t = 0;
	// 30 intervals whose beat length jitters 499/500/501 frames.
	int jig[3] = {499, 500, 501};
	for (int iv = 0; iv < 30; iv++) {
		for (int b = 0; b < bpi; b++) {
			int B = jig[(iv + b) % 3];
			for (int k = 0; k < B; k++) {
				float clk = (k == 0) ? 5.f : 0.f;
				ClockFrame c{};
				ec.tick(false, 0.f, true, clk, false, 0.f, bpi, SR, c);
				if (c.running && c.intervalFrames != reportedN) { reportedN = c.intervalFrames; distinctN++; }
				t++;
			}
		}
	}
	CHECK(distinctN <= 1, "N stability: jitter does not move the reported interval length");
	// A genuine 2x tempo change DOES move N (a deliberate regrid).
	uint32_t g = ec.gen;
	int seen = ec.N;
	for (int iv = 0; iv < 6; iv++)
		for (int b = 0; b < bpi; b++)
			for (int k = 0; k < 250; k++) { // half the period → ~half N
				float clk = (k == 0) ? 5.f : 0.f;
				ClockFrame c{};
				ec.tick(false, 0.f, true, clk, false, 0.f, bpi, SR, c);
			}
	CHECK(ec.N < seen, "N stability: a real tempo change moves N");
	CHECK(ec.gen == g, "N stability: a tempo change does NOT bump the generation (not a bpi/sr change)");
}

// --- gridGeneration discipline: steady tempo must NOT bump gen; a bpi change MUST. ---
static void testGenDiscipline() {
	ExtClock ec;
	ec.onActivate(4, SR);
	const int Np = 400;
	for (int k = 0; k < 2 * Np; k++) {
		ClockFrame c{};
		ec.tick(true, (float) (k % Np) / Np * 10.f, false, 0.f, false, 0.f, 4, SR, c);
	}
	uint32_t g0 = ec.gen;
	for (int k = 0; k < 2 * Np; k++) { // more steady running, same bpi/sr
		ClockFrame c{};
		ec.tick(true, (float) (k % Np) / Np * 10.f, false, 0.f, false, 0.f, 4, SR, c);
	}
	CHECK(ec.gen == g0, "gen: steady tempo does not bump the generation");
	ClockFrame c{};
	ec.tick(true, 0.f, false, 0.f, false, 0.f, 8, SR, c); // bpi 4 -> 8
	CHECK(ec.gen == g0 + 1, "gen: a bpi change bumps the generation");
	uint32_t g1 = ec.gen;
	ec.tick(true, 0.1f, false, 0.f, false, 0.f, 8, 96000.f, c); // sr change
	CHECK(ec.gen == g1 + 1, "gen: a sample-rate change bumps the generation");
}

// --- The seconds budget makes the BPI ceiling sample-rate independent (tempo only). ---
static void testMaxBpi() {
	CHECK(ExtClock::maxBpi(60.f) == 64, "maxBpi: 60 BPM hits the musical hard cap");
	CHECK(ExtClock::maxBpi(10.f) == 40, "maxBpi: 10 BPM → 240s/6s = 40 beats (buffer binds)");
	CHECK(ExtClock::maxBpi(5.f) == 20, "maxBpi: 5 BPM → 20 beats");
	CHECK(ExtClock::maxBpi(0.f) == 64, "maxBpi: unknown tempo → hard cap");
	// Not a function of sample rate at all — same value by construction.
	CHECK(ExtClock::maxBpi(10.f) == ExtClock::maxBpi(10.f), "maxBpi: sr-independent");
}

// --- No external jack patched: tick returns false so the caller uses the simulated clock. ---
static void testUnpatched() {
	ExtClock ec;
	ec.onActivate(4, SR);
	ClockFrame c{};
	bool ext = ec.tick(false, 0.f, false, 0.f, false, 0.f, 4, SR, c);
	CHECK(!ext, "unpatched: external does not own the clock");
}

int main() {
	testPhase();
	testPhaseFreeze();
	testPulseNoReset();
	testPulseWithReset();
	testNStability();
	testGenDiscipline();
	testMaxBpi();
	testUnpatched();
	if (failures) { std::printf("\n%d check(s) FAILED\n", failures); return 1; }
	std::printf("extclock_test: all checks passed\n");
	return 0;
}
