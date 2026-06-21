// Round-trip test for the OGG Vorbis encoder (transmit path): encode a stereo sine tone
// interval, then decode it back with stb_vorbis and check it survived.
//
// Build (from plugin root): compile the vendored ogg/vorbis .c as C, the rest as C++:
//   see test/build_enc_test.sh
//
#include "../src/net/ninjam/NjEncoder.hpp"
#include "../src/dep/stb_vorbis_impl.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main() {
	const int rate = 48000, ch = 2, frames = rate; // 1 second
	std::vector<float> in((size_t) frames * ch);
	for (int i = 0; i < frames; i++) {
		float s = 0.5f * std::sin(2.f * 3.14159265f * 440.f * i / rate); // L: 440 Hz
		float t = 0.4f * std::sin(2.f * 3.14159265f * 660.f * i / rate); // R: 660 Hz
		in[(size_t) i * 2] = s;
		in[(size_t) i * 2 + 1] = t;
	}

	std::vector<uint8_t> ogg = akaudio::nj::encodeOggInterval(in.data(), frames, ch, rate, 0.4f, 1);
	std::printf("encoded: %zu bytes (%.1f kbps)\n", ogg.size(), ogg.size() * 8.0 / 1000.0);
	if (ogg.empty()) { std::printf("FAIL: empty encode\n"); return 1; }

	int dch = 0, drate = 0;
	short* outp = nullptr;
	int dframes = stb_vorbis_decode_memory(ogg.data(), (int) ogg.size(), &dch, &drate, &outp);
	std::printf("decoded: frames=%d channels=%d rate=%d\n", dframes, dch, drate);
	if (dframes <= 0 || !outp) { std::printf("FAIL: decode failed\n"); return 1; }

	double peakL = 0, peakR = 0, sumSq = 0;
	for (int i = 0; i < dframes; i++) {
		double l = outp[(size_t) i * dch] / 32768.0;
		double r = dch >= 2 ? outp[(size_t) i * dch + 1] / 32768.0 : l;
		peakL = std::fmax(peakL, std::fabs(l));
		peakR = std::fmax(peakR, std::fabs(r));
		sumSq += l * l + r * r;
	}
	double rms = std::sqrt(sumSq / (2.0 * dframes));
	std::printf("decoded peakL=%.3f peakR=%.3f rms=%.3f (expect ~0.5/0.4, rms~0.32)\n", peakL, peakR, rms);
	std::free(outp);

	bool ok = dch == 2 && drate == rate && std::abs(dframes - frames) < rate / 10 && peakL > 0.3 && peakR > 0.2;
	std::printf("%s\n", ok ? "PASS: encode->decode round-trip OK" : "FAIL: round-trip mismatch");
	return ok ? 0 : 1;
}
