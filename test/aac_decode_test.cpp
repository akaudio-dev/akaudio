// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov
//
// Offline decode check for the portable (FAAD2) AacDecoder path used on
// Windows/Linux. Feeds a raw ADTS-AAC file through AacDecoder in small chunks
// (mimicking the streaming socket reads) and reports frames decoded, the output
// sample rate (which HE-AAC's SBR doubles), channel fan-out, and RMS level.
//
// Exit 0 only if real, non-silent audio came out — so it doubles as a smoke test.
//
// Build (mingw / linux), reusing the plugin's already-compiled FAAD2 objects:
//   g++ -std=c++11 -I src test/aac_decode_test.cpp src/net/AacDecoder.cpp \
//       build/src/dep/faad2/libfaad/*.o -o build/aac_decode_test
//   build/aac_decode_test heaac.aac

#include "net/AacDecoder.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s file.aac\n", argv[0]);
		return 2;
	}
	std::FILE* f = std::fopen(argv[1], "rb");
	if (!f) {
		std::perror("open");
		return 2;
	}

	akaudio::AacDecoder dec;
	long totalFrames = 0;
	double outRate = 0;
	int lastCh = 0;
	double sumsq = 0;
	long nSamp = 0;

	dec.onPCM = [&](const float* pcm, int frames, double srcRate) {
		totalFrames += frames;
		outRate = srcRate;
		lastCh = 2; // AacDecoder always emits interleaved stereo
		for (int i = 0; i < frames * 2; i++) {
			sumsq += (double) pcm[i] * pcm[i];
			nSamp++;
		}
	};

	if (!akaudio::AacDecoder::available()) {
		std::fprintf(stderr, "AAC decoder not available on this build\n");
		return 1;
	}
	if (!dec.init()) {
		std::fprintf(stderr, "AAC init failed\n");
		return 1;
	}

	std::vector<unsigned char> buf(4096);
	size_t rd;
	while ((rd = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
		if (!dec.feed(buf.data(), rd)) {
			std::fprintf(stderr, "feed reported fatal error\n");
			break;
		}
	}
	dec.close();
	std::fclose(f);

	double rms = nSamp ? std::sqrt(sumsq / nSamp) : 0;
	double secs = outRate > 0 ? totalFrames / outRate : 0;
	std::printf("decoded frames=%ld  outRate=%.0f Hz  outCh=%d  seconds=%.2f  rms=%.5f\n",
		totalFrames, outRate, lastCh, secs, rms);

	bool ok = totalFrames > 0 && rms > 1e-4;
	std::printf("%s\n", ok ? "OK: real audio decoded" : "FAIL: no/silent audio");
	return ok ? 0 : 1;
}
