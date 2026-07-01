// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

// Leak / use-after-free stress for StreamClient: churn the full lifecycle
// (construct -> start -> decode -> stop -> destruct) many times over BOTH decode
// paths (MP3 via dr_mp3, AAC/HLS via AudioToolbox), so a per-cycle leak accumulates
// and a stop-path use-after-free trips a sanitizer. Each station switch in the real
// plugin is exactly one of these cycles, so this models the heaviest real usage.
//
// Built + run by `make leakcheck` (which links the already-compiled objects and runs
// it under Apple's `leaks`). Two ways to use it:
//   * leaks: `leaks --atExit -- ./leak_stress 5` — expect "0 leaks". Run at two cycle
//     counts; a flat node/byte total proves there is no per-cycle accumulation.
//   * ASan: build with -fsanitize=address and run — catches use-after-free / overflow
//     in the threaded start/stop path (Apple clang has no LeakSanitizer on arm64, so
//     `leaks` covers leak detection and ASan covers memory-safety).
//
// Needs internet (it streams live public URLs). No Rack, no GUI.

#include "../src/net/Stream.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

static void pump(akaudio::StreamClient& s, int ms) {
	const int frames = (int) (48000.0 * ms / 1000.0);
	for (int i = 0; i < frames; i++) {
		float l, r;
		s.pull(l, r); // drain like the audio thread would
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int main(int argc, char** argv) {
	int cycles = argc > 1 ? std::atoi(argv[1]) : 4;
	const std::vector<std::string> urls = {
		"http://ninbot.com/radio/2049",                                    // MP3 (Icecast)
		"http://as-hls-ww-live.akamaized.net/pool_55057080/live/ww/"
		"bbc_radio_fourfm/bbc_radio_fourfm.isml/"
		"bbc_radio_fourfm-audio%3d128000.norewind.m3u8",                   // HLS -> AAC
	};
	for (int c = 0; c < cycles; c++) {
		for (const std::string& u : urls) {
			akaudio::StreamClient s;       // construct
			s.setSampleRate(48000.f);
			s.start(u);                    // spin bg thread: TLS/HTTP/decoder/resampler
			pump(s, 1200);                 // let it connect + actually decode
			s.stop();                      // join bg thread, free decoder/TLS
			std::printf("cycle %d  %-20s  produced=%lld\n", c, u.substr(7, 16).c_str(),
				(long long) s.producedFrames());
		}                                  // destruct
	}
	std::printf("done — %d cycles, no crash\n", cycles);
	return 0;
}
