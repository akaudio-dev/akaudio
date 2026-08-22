// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

// Offline test for JamClock (the integer-frame interval grid shared by Ninjam and the
// Looper expanders). No Rack link.
//   c++ -std=c++11 -I src test/jamclock_test.cpp -o build/jamclock_test && build/jamclock_test
#include "JamClock.hpp"
#include <cstdio>
#include <cstdlib>

using akaudio::JamClock;
using akaudio::JamClockMessage;

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
	// 130 BPM, 8 BPI @44.1k: N is NOT an integer number of beats*spb (162830.77) — the
	// case where the old fractional clock drifted. Integer N must match NjAudio's formula.
	CHECK(JamClock::intervalFrames(130, 8, 44100.0) == 162831, "N@130/8/44.1k");
	CHECK(JamClock::intervalFrames(120, 32, 48000.0) == 768000, "N@120/32/48k");
	CHECK(JamClock::intervalFrames(1, 32, 48000.0) == 0, "broken tempo capped to 0");
	CHECK(JamClock::intervalFrames(0, 8, 48000.0) == 0, "bpm 0");

	JamClock c;
	JamClockMessage m = c.tick();
	CHECK(!m.running && m.intervalFrames == 0, "idle clock reports !running");

	c.regrid(130, 8, 44100.f);
	const int N = c.N;
	CHECK(c.running && N == 162831, "regrid runs");
	CHECK(c.gen == 1, "gen++ on regrid");

	// Walk 3 intervals: exactly one downbeat per interval at frame 0, exactly bpi beats
	// per interval, beatIndex monotonic 0..bpi-1, frame counts 0..N-1, session monotonic.
	int downbeats = 0, beats = 0, lastBeat = -1;
	uint64_t lastSession = 0;
	for (int i = 0; i < 3 * N; i++) {
		m = c.tick();
		CHECK(m.running, "running during walk");
		CHECK(m.frameInInterval == i % N, "frame %d -> %d", i, m.frameInInterval);
		CHECK(m.downbeat == (i % N == 0), "downbeat flag at %d", i);
		if (m.downbeat) { downbeats++; CHECK(m.beatIndex == 0, "downbeat is beat 0"); }
		if (m.beat) {
			beats++;
			int expect = (lastBeat + 1) % 8;
			CHECK(m.beatIndex == expect, "beat order at %d: got %d want %d", i, m.beatIndex, expect);
			lastBeat = m.beatIndex;
		}
		CHECK(m.beatIndex >= 0 && m.beatIndex < 8, "beatIndex range");
		CHECK(i == 0 || m.sessionFrame == lastSession + 1, "session monotonic at %d", i);
		lastSession = m.sessionFrame;
		double bf = JamClock::beatFrac(m);
		CHECK(bf >= 0.0 && bf <= 1.0, "beatFrac range");
	}
	CHECK(downbeats == 3, "3 downbeats in 3 intervals (got %d)", downbeats);
	CHECK(beats == 24, "24 beats in 3 intervals (got %d)", beats);
	CHECK(lastSession == (uint64_t) (3 * N - 1), "session frame = frames ticked - 1");

	// Tempo change while running: regrid keeps the timeline, bumps gen, rephases to 0.
	uint64_t before = c.session;
	c.regrid(120, 32, 44100.f);
	m = c.tick();
	CHECK(c.gen == 2, "gen++ on tempo change");
	CHECK(m.downbeat && m.beat && m.frameInInterval == 0, "tempo change rephases to frame 0");
	CHECK(m.sessionFrame == before, "timeline continues across a tempo change");
	CHECK(m.intervalFrames == JamClock::intervalFrames(120, 32, 44100.0), "new N");

	// Leave + rejoin: stop() then regrid() starts a new session at 0.
	c.stop();
	m = c.tick();
	CHECK(!m.running && c.gen == 3, "stop: !running, gen++");
	c.regrid(100, 16, 48000.f);
	m = c.tick();
	CHECK(m.running && m.sessionFrame == 0 && m.downbeat, "rejoin: new session from 0");
	c.stop(); c.stop();
	CHECK(c.gen == 5, "stop is idempotent for gen (got %u)", c.gen);

	std::printf("%s (%d failures)\n", fails ? "FAIL" : "PASS: JamClock", fails);
	return fails ? 1 : 0;
}
