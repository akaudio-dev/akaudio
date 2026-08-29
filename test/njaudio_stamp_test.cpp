// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov
//
// Offline check of the wire-archive stamping (LOOPER_DESIGN §7.3): received intervals
// are handed to onIntervalReceived at their chained playout START on the shared session
// timeline — not at network arrival. Drives NjAudio the way the module does (feed
// intervals on a "net thread", freewheel-pull wide frames while publishing the session
// clock like process() does) and asserts:
//   - one callback per real interval, bytes verbatim, tempo metadata intact;
//   - consecutive stamps exactly one interval apart (chained — arrival jitter erased);
//   - the first stamp sits within the arrival+jitter-hold window, on the session axis
//     (i.e. the published base offset is applied via pullOffset);
//   - a silence interval fires nothing.
//
// Build (see `make unittest`):
//   c++ -std=c++11 -O1 -I src -I src/dep/libogg/include -I src/dep/libvorbis/include \
//     -I src/dep/libvorbis/lib test/njaudio_stamp_test.cpp src/net/ninjam/NjAudio.cpp \
//     src/net/ninjam/NjEncoder.cpp src/dep/stb_vorbis_impl.cpp <ogg+vorbis objs> \
//     -lpthread -o build/njaudio_stamp_test && build/njaudio_stamp_test
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/ninjam/NjAudio.hpp"
#include "net/ninjam/NjEncoder.hpp"

static int failures = 0;
#define CHECK(cond, msg) \
	do { \
		if (cond) std::printf("  ok: %s\n", msg); \
		else { std::printf("  FAIL: %s\n", msg); failures++; } \
	} while (0)

int main() {
	using namespace akaudio::nj;
	const int SR = 48000, BPM = 120, BPI = 8;
	const int N = 192000; // BPI * 60/BPM * SR — one interval
	const uint64_t BASE = 500000; // pretend the engine joined mid-session

	// One interval of a −12 dB sine, encoded like a real sender's upload.
	std::vector<float> pcm((size_t) N * 2);
	for (int i = 0; i < N; i++) {
		float s = 0.25f * std::sin(2.0 * 3.14159265358979 * 220.0 * i / SR);
		pcm[(size_t) i * 2] = s;
		pcm[(size_t) i * 2 + 1] = s;
	}
	std::vector<uint8_t> ogg = encodeOggInterval(pcm.data(), N, 2, SR, 0.5f, 7);
	if (ogg.empty()) { std::printf("FAIL: encoder produced nothing\n"); return 1; }

	NjAudio a;
	a.setSampleRate(SR);
	a.setTempo(BPM, BPI);

	std::mutex smu;
	std::vector<uint64_t> stamps;
	std::vector<size_t> lens;
	int lastBpm = 0, lastBpi = 0, lastFrames = 0;
	std::string lastUser;
	a.onIntervalReceived = [&](const std::string& user, int chidx, const uint8_t* bytes,
	                           size_t len, int bpm, int bpi, int frames, uint64_t sf) {
		(void) chidx; (void) bytes;
		std::lock_guard<std::mutex> lk(smu);
		stamps.push_back(sf);
		lens.push_back(len);
		lastBpm = bpm; lastBpi = bpi; lastFrames = frames; lastUser = user;
	};
	a.start();
	a.onUserChannel("bob", 0, true, 0, 0);

	// The tempo is applied by mixLoop's first pass — wait for the grid before feeding.
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	int b, i, frames = 0;
	while (frames != N && std::chrono::steady_clock::now() < deadline) {
		a.currentTempo(b, i, frames);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(frames == N, "interval grid live (192000 frames)");

	// Three real intervals, delivered back-to-back (arrival time deliberately carries
	// no grid information), then a silence interval (zero guid, no archive callback).
	const uint32_t OGGV = 'O' | ('G' << 8) | ('G' << 16) | ('v' << 24);
	for (int k = 0; k < 3; k++) {
		uint8_t guid[16] = {0};
		guid[0] = (uint8_t) (k + 1);
		a.beginInterval("bob", 0, guid, OGGV);
		a.writeInterval(guid, ogg.data(), ogg.size(), true);
	}
	uint8_t zero[16] = {0};
	a.beginInterval("bob", 0, zero, 0);

	// "Audio thread": freewheel-pull wide frames, publishing session = BASE + pulled
	// exactly like Ninjam::process() publishes its JamClock each frame. pullOffset is
	// then BASE, so stamps land on the session axis deterministically.
	a.publishSession(BASE);
	float frame[RING_CH];
	uint64_t pulled = 0;
	const uint64_t NEED = (uint64_t) N * 4; // enough to play all three + slack
	deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (pulled < NEED && std::chrono::steady_clock::now() < deadline) {
		if (a.pullFrame(frame)) {
			pulled++;
			a.publishSession(BASE + pulled);
		} else {
			std::this_thread::sleep_for(std::chrono::microseconds(200));
		}
	}
	a.stop();

	std::lock_guard<std::mutex> lk(smu);
	CHECK(stamps.size() == 3, "three real intervals archived, silence fired nothing");
	if (stamps.size() == 3) {
		CHECK(lens[0] == ogg.size() && lens[1] == ogg.size() && lens[2] == ogg.size(),
		      "raw bytes delivered verbatim");
		CHECK(lastUser == "bob" && lastBpm == BPM && lastBpi == BPI && lastFrames == N,
		      "tempo metadata intact");
		CHECK(stamps[1] - stamps[0] == (uint64_t) N && stamps[2] - stamps[1] == (uint64_t) N,
		      "chained stamps exactly one interval apart (no arrival jitter)");
		// First stamp: after the startup jitter hold (min(1 s, N/4) = 48000 here), on
		// the session axis (>= BASE), and within a loose arrival window.
		CHECK(stamps[0] >= BASE && stamps[0] < BASE + 2 * (uint64_t) N,
		      "first stamp on the session axis, within the arrival window");
		CHECK(stamps[0] != UINT64_MAX, "pullOffset was published before the first pop");
	}

	// Flush-on-stop: an interval fully received but never played (the jam's last
	// chord, still queued behind the playing one at teardown) must STILL reach the
	// archive — delivered by flushOrphans with the UINT64_MAX clock-fallback stamp.
	{
		NjAudio a2;
		a2.setSampleRate(SR);
		a2.setTempo(BPM, BPI);
		std::mutex m2;
		std::vector<uint64_t> st2;
		a2.onIntervalReceived = [&](const std::string&, int, const uint8_t*, size_t,
		                            int, int, int, uint64_t sf) {
			std::lock_guard<std::mutex> lk(m2);
			st2.push_back(sf);
		};
		a2.start();
		a2.onUserChannel("bob", 0, true, 0, 0);
		int b2, i2, f2 = 0;
		auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (f2 != N && std::chrono::steady_clock::now() < dl) {
			a2.currentTempo(b2, i2, f2);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		for (int k = 0; k < 2; k++) {
			uint8_t guid[16] = {0};
			guid[0] = (uint8_t) (k + 1);
			a2.beginInterval("bob", 0, guid, OGGV);
			a2.writeInterval(guid, ogg.data(), ogg.size(), true);
		}
		a2.publishSession(BASE);
		float fr[RING_CH];
		uint64_t pulled2 = 0;
		dl = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (pulled2 < (uint64_t) N / 2 && std::chrono::steady_clock::now() < dl) {
			// Half an interval: the first interval starts (archived at its playout
			// start), the second stays queued when we tear down.
			if (a2.pullFrame(fr)) {
				pulled2++;
				a2.publishSession(BASE + pulled2);
			} else {
				std::this_thread::sleep_for(std::chrono::microseconds(200));
			}
		}
		a2.stop();
		std::lock_guard<std::mutex> lk(m2);
		CHECK(st2.size() == 2, "flush-on-stop: both intervals reached the archive");
		if (st2.size() == 2) {
			CHECK(st2[0] != UINT64_MAX, "flush-on-stop: the played one carries a playout stamp");
			CHECK(st2[1] == UINT64_MAX, "flush-on-stop: the unplayed one carries the clock-fallback stamp");
		}
	}

	std::printf(failures ? "\n%d FAILURES\n" : "\nall checks passed\n", failures);
	return failures ? 1 : 0;
}
