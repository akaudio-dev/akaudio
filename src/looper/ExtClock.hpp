// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
// ExtClock — the Looper's standalone external clock source (docs/LOOPER_DESIGN.md §3.5).
//
// Turns either a continuous PHASE ramp (ZZC-style 0–10 V saw, one cycle per interval) or
// a CLOCK + RESET pulse pair into the same `ClockFrame` the Ninjam expander reader and the
// simulated clock fill — so the engine is untouched and this is just a third source, which
// the Looper selects when no Ninjam clock is live but a clock cable is patched (precedence
// Ninjam → PHASE → CLOCK → simulated).
//
// Rack-free on purpose (no `rack::dsp`, no Rack headers beyond ClockFrame): the edge
// detector is a hand-rolled Schmitt trigger, so test/extclock_test.cpp exercises it with
// no Rack link, exactly like JamClock.
//
// Design points, all from §3.5:
//  - PHASE is preferred: the ramp IS the position (frameInInterval = round(phase·N)); the
//    wrap is the downbeat; N is latched to the measured cycle length. No period estimation.
//  - CLOCK+RESET reconstructs the grid from pulses: CLOCK edges are beats, RESET is the
//    downbeat (interval boundary); with no RESET patched the interval is `bpi` beats long.
//  - The interval cap is a SECONDS budget (MAX_INTERVAL_SECONDS → N_max = round(s·sr)), so
//    the maximum loop length is the same musical duration at every sample rate; the BPI
//    ceiling the caller offers the stepper is therefore tempo-dependent but sr-independent
//    (see maxBpi()).
//  - Clock stop = freeze: when the source goes idle (phase stationary / no CLOCK edge for
//    > 2 beats) we report running=false and hold the position, so the engine pauses
//    playback (its playback gate is c.running) instead of drifting.
//  - gridGeneration bumps only on bpi change, sample-rate change and (re)activation — never
//    on ordinary tempo drift, which must not tear down in-flight recordings/chains.
#include "LooperEngine.hpp" // ClockFrame
#include <cmath>
#include <cstdint>

namespace akaudio {
namespace looper {

struct ExtClock {
	static constexpr float MAX_INTERVAL_SECONDS = 240.f; // seconds budget → N cap (memory-bounded; sr-independent in beats)
	static constexpr int   MIN_BPI = 3;                  // 1–2 are musically meaningless (3/4, 3/8 are real)
	static constexpr int   HARD_MAX_BPI = 64;            // musical cap for the stepper, further limited by the buffer

	// ---- UI-facing ----
	uint32_t gen = 500000;  // ClockFrame::gridGeneration source; bumps on bpi/sr change + activation
	bool stopped = false;   // external clock patched but idle (frozen) this frame

	// ---- measurement state ----
	bool clockHi = false, resetHi = false;
	int64_t frame = 0;          // monotonic frame counter while external is patched
	int64_t lastClock = -1;     // frame of the last CLOCK rising edge
	int64_t lastReset = -1;     // frame of the last RESET rising edge
	int     beatFrames = 0;     // last measured CLOCK period (frames)
	int64_t downbeatFrame = -1; // frame the current interval's downbeat landed on
	int     pulseBeat = 0;      // beat index since the last downbeat (no-RESET mode)
	float   lastPhase = -1.f;   // previous PHASE reading (−1 = none yet)
	int64_t phaseWrap = -1;     // frame of the last PHASE wrap
	int     phaseN = 0;         // interval length latched from the last full PHASE cycle
	int     stationary = 0;     // consecutive frames PHASE has not moved (freeze detect)

	// ---- grid being reported ----
	int      N = 0;             // STABLE reported interval length (hysteresis — see stabilizeN)
	int      resetPeriod = 0;   // last measured RESET-to-RESET length (pulse mode)
	int      frameInInterval = 0;
	int      lastBeatIndex = -1;
	uint64_t session = 0;
	int      lastBpi = -1;
	float    lastSr = 0.f;

	static int clampBpi(int bpi) {
		if (bpi < MIN_BPI) return MIN_BPI;
		if (bpi > HARD_MAX_BPI) return HARD_MAX_BPI;
		return bpi;
	}
	// The ceiling the Looper offers its BPI stepper: the musical hard cap, further limited
	// so one interval fits the seconds budget at the current beat length. `beatSeconds`
	// cancels the sample rate, so this is tempo-dependent only. bpm ≤ 0 ⇒ the hard cap.
	static int maxBpi(float bpm) {
		if (bpm <= 0.f) return HARD_MAX_BPI;
		double beatSeconds = 60.0 / (double) bpm;
		int byBuffer = (int) std::floor((double) MAX_INTERVAL_SECONDS / beatSeconds);
		int m = byBuffer < HARD_MAX_BPI ? byBuffer : HARD_MAX_BPI;
		return m < MIN_BPI ? MIN_BPI : m;
	}

	// Reset transient measurement state when (re)entering external mode. Bumps the
	// generation so the engine regrids on the source switch.
	void onActivate(int bpi, float sr) {
		clockHi = resetHi = false;
		frame = 0; lastClock = lastReset = -1; beatFrames = 0; downbeatFrame = -1; pulseBeat = 0;
		lastPhase = -1.f; phaseWrap = -1; phaseN = 0; stationary = 0;
		N = 0; resetPeriod = 0; frameInInterval = 0; lastBeatIndex = -1;
		stopped = false; lastBpi = clampBpi(bpi); lastSr = sr;
		gen++;
	}

	// Rising edge with hysteresis (~1 V hi / 0.1 V lo).
	static bool rising(bool& hi, float v) {
		if (!hi && v > 1.f) { hi = true; return true; }
		if (hi && v < 0.1f) hi = false;
		return false;
	}

	// Produce this frame's clock. Returns true when EXTERNAL owns the clock (a cable is
	// patched) — `out` is filled and the caller must use it (out.running may be false while
	// warming up or frozen, which pauses the engine). Returns false only when no external
	// jack is patched, so the caller falls back to the simulated clock.
	bool tick(bool phasePatched, float phaseV,
	          bool clockPatched, float clockV,
	          bool resetPatched, float resetV,
	          int bpiIn, float sr, ClockFrame& out) {
		const int bpi = clampBpi(bpiIn);
		if (bpi != lastBpi || sr != lastSr) { gen++; lastBpi = bpi; lastSr = sr; lastBeatIndex = -1; }
		const int Nmax = (int) std::lround((double) MAX_INTERVAL_SECONDS * (double) sr);

		if (!phasePatched && !clockPatched)
			return false; // not external — caller uses the simulated clock

		bool running;
		int beatIndex = 0;
		bool downbeat = false;
		// CLOCK (+ RESET) is the primary source: when a clock cable is patched it wins, even
		// if PHASE is also patched. PHASE drives ONLY when there is no CLOCK (a pure phase-ramp
		// rig). (Precedence reworked 2026-10-04 — CLOCK-first is far more predictable: most
		// clock modules emit gate pulses, not a 0-10 V ramp, so a non-ramp signal on PHASE must
		// not hijack a working CLOCK+RESET setup and freeze the grid.)
		if (clockPatched)
			running = tickPulse(clockV, resetPatched, resetV, bpi, Nmax, beatIndex, downbeat);
		else
			running = tickPhase(phaseV, bpi, Nmax, beatIndex, downbeat);

		// Beat flag: the beat index changed this frame (incl. the downbeat).
		bool beat = running && (downbeat || beatIndex != lastBeatIndex);
		if (running) lastBeatIndex = beatIndex; else lastBeatIndex = -1;

		out.sampleRate = sr;
		out.gridGeneration = gen;
		out.running = running;
		if (running) {
			out.intervalFrames = N;
			out.frameInInterval = frameInInterval;
			out.beatIndex = beatIndex;
			out.downbeat = downbeat;
			out.beat = beat;
			out.bpi = bpi;
			double beatSeconds = (double) N / (double) bpi / (double) sr;
			out.bpm = beatSeconds > 0.0 ? (int) std::lround(60.0 / beatSeconds) : 0;
			out.sessionFrame = session++;
		} else {
			// Warming up or frozen: hold. N=0 keeps the engine idle without a regrid.
			out.intervalFrames = 0;
			out.frameInInterval = frameInInterval;
			out.beatIndex = 0;
			out.downbeat = false;
			out.beat = false;
			out.bpm = 0;
			out.bpi = bpi;
			out.sessionFrame = session; // not advanced
		}
		frame++;
		return true;
	}

private:
	// The engine full-regrids (cancels pending actions, discards in-flight recordings, kills
	// auto-advance chains) on ANY change of intervalFrames — the other clock sources keep N
	// rock-stable, so a measured period that jitters ±1 frame would regrid every interval.
	// Hold N through small jitter (~0.8 %); only a real tempo change (a large delta) moves
	// it, which then IS a deliberate regrid, exactly like a Ninjam server tempo change.
	int stabilizeN(int candidate, int Nmax) {
		int c = candidate > Nmax ? Nmax : candidate;
		if (c < 1) return N; // keep the last good value
		if (N == 0) { N = c; return N; }
		int tol = N / 128; if (tol < 2) tol = 2;
		if (std::abs(c - N) > tol) N = c;
		return N;
	}

	// PHASE: a 0–10 V ramp, one cycle per interval. The ramp is the position; the wrap is
	// the downbeat; N is the measured cycle length (slope-estimated before the first wrap).
	bool tickPhase(float v, int bpi, int Nmax, int& beatIndex, bool& downbeat) {
		float p = v * 0.1f;
		if (p < 0.f) p = 0.f;
		if (p > 1.f) p = 1.f;

		if (lastPhase < 0.f) { lastPhase = p; stationary = 0; stopped = false; return false; }
		float dp = p - lastPhase;

		if (dp < -0.5f) { // wrap → downbeat; latch the cycle length
			if (phaseWrap >= 0) {
				int period = (int) (frame - phaseWrap);
				if (period >= 1) phaseN = period;
			}
			phaseWrap = frame;
			downbeat = true;
		}
		// Freeze: the ramp is genuinely HELD (transport stopped). The threshold is near float
		// epsilon so only an unchanging value counts — a slow-but-moving ramp (even a
		// multi-minute interval) keeps a dp above it and is NOT mistaken for stopped.
		stationary = (std::fabs(dp) < 1e-7f) ? stationary + 1 : 0;
		stopped = stationary > (int) std::lround(lastSr * 0.5); // held ~0.5 s = stopped

		// N: the latched cycle, or a slope estimate until the first wrap lands.
		int n = phaseN;
		if (n <= 0 && dp > 1e-6f) n = (int) std::lround(1.0 / dp);
		lastPhase = p;
		if (n <= 0 || stopped) return false; // warming up or frozen
		stabilizeN(n, Nmax);

		int fi = (int) std::lround((double) p * (double) N);
		if (fi >= N) fi = N - 1;
		if (fi < 0) fi = 0;
		frameInInterval = fi;
		beatIndex = (int) ((double) p * (double) bpi);
		if (beatIndex >= bpi) beatIndex = bpi - 1;
		if (beatIndex < 0) beatIndex = 0;
		return true;
	}

	// CLOCK + RESET: CLOCK edges are beats, RESET is the downbeat. With no RESET patched the
	// interval is `bpi` beats and the downbeat is every bpi-th CLOCK edge. frameInInterval is
	// counted from the downbeat; N is the measured interval (RESET period, or bpi·beat).
	bool tickPulse(float clockV, bool resetPatched, float resetV, int bpi, int Nmax,
	               int& beatIndex, bool& downbeat) {
		bool clockEdge = rising(clockHi, clockV);
		bool resetEdge = resetPatched && rising(resetHi, resetV);

		if (resetEdge) {
			if (lastReset >= 0) { int per = (int) (frame - lastReset); if (per >= 1) resetPeriod = per; }
			lastReset = frame;
			downbeatFrame = frame;
			pulseBeat = 0;
			downbeat = true;
		}
		if (clockEdge) {
			if (lastClock >= 0) { int bf = (int) (frame - lastClock); if (bf >= 1) beatFrames = bf; }
			lastClock = frame;
			if (resetEdge) {
				// coincident reset already set the downbeat
			} else if (downbeatFrame < 0) {
				// first beat ever: treat it as the downbeat so the grid has an anchor
				downbeatFrame = frame; pulseBeat = 0; downbeat = true;
			} else if (!resetPatched) {
				pulseBeat++;
				if (pulseBeat >= bpi) { pulseBeat = 0; downbeatFrame = frame; downbeat = true; }
			} else {
				if (pulseBeat < bpi - 1) pulseBeat++; // RESET owns the boundary; just advance the display
			}
		}

		// Interval length: RESET period when patched, else bpi beats.
		int n = resetPatched ? resetPeriod : bpi * beatFrames;

		// Freeze: no CLOCK edge for more than two beats.
		stopped = beatFrames > 0 && lastClock >= 0 && (frame - lastClock) > 2LL * beatFrames;

		if (beatFrames <= 0 || downbeatFrame < 0 || n <= 0 || stopped)
			return false; // warming up or frozen
		stabilizeN(n, Nmax);

		int fi = (int) (frame - downbeatFrame);
		if (fi >= N) fi = N - 1; // clock slower than N implies: hold at the boundary
		if (fi < 0) fi = 0;
		frameInInterval = fi;
		// The action grid is BPI even subdivisions of the interval (like Ninjam and PHASE) —
		// NOT the raw CLOCK-edge count. So BPI, not the clock rate, sets the beat granularity:
		// clock CLK at any rate (even one pulse per interval, with RESET marking the loop) and
		// still get BPI launch/record points per interval. `pulseBeat` only locates the
		// downbeat in the no-RESET case now.
		beatIndex = (int) ((long long) fi * bpi / N);
		if (beatIndex >= bpi) beatIndex = bpi - 1;
		if (beatIndex < 0) beatIndex = 0;
		return true;
	}
};

} // namespace looper
} // namespace akaudio
