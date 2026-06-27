// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrey Kozlov

// Standalone smoke test for the net/ audio path: drive StreamClient against a
// live HTTP/Icecast MP3 stream (default: a ninbot NINJAM room) and confirm we
// actually decode non-silent audio at roughly real-time.
//
// Build + run (from the plugin root; R = your RACK_DIR):
//   c++ -std=c++11 -I src -I "$R/dep/include" \
//     test/play_test.cpp src/net/Stream.cpp src/net/Tls.cpp \
//     src/net/AacDecoder.cpp src/net/Http.cpp src/dep/dr_mp3_impl.cpp \
//     "$R/dep/lib/libssl.a" "$R/dep/lib/libcrypto.a" \
//     -framework AudioToolbox -framework CoreFoundation -framework AudioUnit \
//     -o build/play_test
//   build/play_test [url] [seconds]
//
// Links the net layer + dr_mp3 + OpenSSL (https) + AudioToolbox (AAC) — no Rack,
// no jansson — so it isolates "can we play this stream?" from the whole plugin.
// Works for http/https, MP3/AAC, and .pls/.m3u playlist URLs.

#include "../src/net/Stream.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

int main(int argc, char** argv) {
	std::string url = argc > 1 ? argv[1] : "http://ninbot.com/radio/2049";
	double seconds = argc > 2 ? std::atof(argv[2]) : 5.0;
	const double sr = 48000.0;

	std::printf("Stream: %s\n", url.c_str());
	std::printf("Target: %.0f Hz for %.1f s\n\n", sr, seconds);

	akaudio::StreamClient s;
	s.setSampleRate((float) sr);
	s.start(url);

	// Consume at ~real-time so the producer's backpressure behaves like the
	// audio thread would: pull a 10 ms block, sleep 10 ms, repeat.
	const int blockMs = 10;
	const int blockFrames = (int) (sr * blockMs / 1000.0);
	const int totalBlocks = (int) (seconds * 1000.0 / blockMs);

	long pulled = 0, underruns = 0, nonzero = 0;
	double sumSq = 0.0;
	float peak = 0.f;
	std::string lastStatus;

	for (int b = 0; b < totalBlocks; b++) {
		for (int i = 0; i < blockFrames; i++) {
			float l = 0.f, r = 0.f;
			if (s.pull(l, r)) {
				pulled++;
				float m = 0.5f * (l + r);
				if (m != 0.f)
					nonzero++;
				sumSq += (double) m * m;
				float a = std::fabs(m);
				if (a > peak)
					peak = a;
			}
			else {
				underruns++;
			}
		}
		std::string st = s.getStatusText();
		if (st != lastStatus) {
			std::printf("[%4d ms] %s\n", b * blockMs, st.c_str());
			lastStatus = st;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(blockMs));
	}

	s.stop();

	double rms = pulled ? std::sqrt(sumSq / pulled) : 0.0;
	std::printf("\nFrames pulled : %ld\n", pulled);
	std::printf("Underruns     : %ld\n", underruns);
	std::printf("Non-zero      : %ld (%.1f%%)\n", nonzero, pulled ? 100.0 * nonzero / pulled : 0.0);
	std::printf("Peak          : %.4f\n", peak);
	std::printf("RMS           : %.4f\n", rms);

	bool ok = pulled > sr * seconds * 0.5 && nonzero > pulled / 2 && peak > 0.001f;
	std::printf("\n%s\n", ok ? "PASS: decoded real audio" : "FAIL: no/insufficient audio");
	return ok ? 0 : 1;
}
