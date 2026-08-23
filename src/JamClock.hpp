// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
// JamClock — the NINJAM interval grid as ONE integer-frame clock, and the message
// that carries it from Ninjam to its Looper expanders (docs/LOOPER_DESIGN.md §3).
//
// Ninjam's local beat clock is the grid our uploads are cut on and the one the user
// hears as the downbeat. It used to accumulate a fractional samples-per-beat, which
// drifted a sub-sample per interval against the TX encoder's integer interval length
// (NjAudio: N = llround(bpi·60·sr/bpm)). JamClock counts integer frames against that
// same N, so RESET/PHASE, TX arming and every Looper agree exactly.
//
// Threading: JamClock lives on the audio thread (one owner, no atomics). The message
// is a POD copied by value into each Looper's expander buffer once per frame.
#include <cmath>
#include <cstdint>
#include <cstring>

namespace akaudio {

struct JamClockMessage {
	bool running;            // JOIN mode with a tempo (bpm, bpi > 0)
	int bpm, bpi;
	int intervalFrames;      // integer N; 0 if !running
	int frameInInterval;     // 0..N-1 — THE position
	int beatIndex;           // 0..bpi-1
	bool downbeat;           // frameInInterval == 0 this frame (interval boundary)
	bool beat;               // first frame of a beat (incl. the downbeat)
	uint32_t gridGeneration; // ++ on every regrid: join, server tempo change, sample-rate change
	uint64_t sessionFrame;   // monotonic frames since the session (join) started — the shared timeline
	float sampleRate;
	char roomLabel[64];      // NUL-terminated; "" until Ninjam publishes it
};

// (The Looper→Ninjam expander-audio path was removed 2026-08-23: route the Looper's MIX
// OUT to Ninjam's IN through the mixer with a real cable — using the mixer to its fullest
// — rather than an invisible connection. Only the clock travels over the expander now.)

struct JamClock {
	int bpm = 0, bpi = 0;
	float sampleRate = 0.f;
	int N = 0;               // interval length in frames
	int frame = 0;           // frame about to be reported by tick()
	int lastBeat = -1;       // beat index reported by the previous tick (−1 = none yet)
	uint32_t gen = 0;
	uint64_t session = 0;
	bool running = false;

	// Same formula + sanity cap as NjAudio::recomputeIntervalLocked (≈87 s @48k).
	static int intervalFrames(int bpm, int bpi, double sr) {
		if (bpm <= 0 || bpi <= 0 || sr <= 0)
			return 0;
		long long n = std::llround((double) bpi * 60.0 * sr / (double) bpm);
		return (n > (1 << 22)) ? 0 : (int) n;
	}

	// (Re)grid to a tempo at this sample rate and phase to frame 0 — join, server
	// CONFIG_CHANGE, sample-rate change. A regrid from stopped starts a new session
	// (timeline from 0); a regrid while running keeps the timeline (tempo change).
	void regrid(int bpm_, int bpi_, float sr) {
		if (!running)
			session = 0;
		bpm = bpm_;
		bpi = bpi_;
		sampleRate = sr;
		N = intervalFrames(bpm, bpi, sr);
		frame = 0;
		lastBeat = -1;
		running = N > 0;
		gen++;
	}
	void stop() {
		if (!running)
			return;
		running = false;
		bpm = bpi = 0;
		N = 0;
		frame = 0;
		lastBeat = -1;
		gen++;
	}

	// Report this frame, then advance. The first tick after regrid() reports frame 0
	// (downbeat + beat both true).
	JamClockMessage tick() {
		JamClockMessage m;
		std::memset(&m, 0, sizeof(m));
		m.gridGeneration = gen;
		m.sampleRate = sampleRate;
		if (!running)
			return m;
		m.running = true;
		m.bpm = bpm;
		m.bpi = bpi;
		m.intervalFrames = N;
		m.frameInInterval = frame;
		m.beatIndex = (int) ((long long) frame * bpi / N);
		m.downbeat = frame == 0;
		m.beat = m.beatIndex != lastBeat;
		m.sessionFrame = session;
		lastBeat = m.beatIndex;
		if (++frame >= N) {
			frame = 0;
			lastBeat = -1; // the next frame 0 is a fresh beat 0
		}
		session++;
		return m;
	}

	// 0..1 within the current beat (for sub-beat CLOCK pulses).
	static double beatFrac(const JamClockMessage& m) {
		if (!m.running || m.bpi <= 0 || m.intervalFrames <= 0)
			return 0.0;
		double beatLen = (double) m.intervalFrames / (double) m.bpi;
		double beatStart = m.beatIndex * beatLen;
		double f = ((double) m.frameInInterval - beatStart) / beatLen;
		return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
	}
	static float intervalPhase(const JamClockMessage& m) {
		return m.intervalFrames > 0 ? (float) m.frameInInterval / (float) m.intervalFrames : 0.f;
	}
};

} // namespace akaudio
