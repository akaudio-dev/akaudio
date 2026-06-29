// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

// Standalone harness for the NINJAM protocol client (Phases 1-2): connect to a
// server, anonymous SHA1 auth, keepalive, and print the live room metadata
// (tempo from CONFIG_CHANGE, roster from USERINFO_CHANGE). No audio decode yet
// (Phase 3). No Rack, no GUI — isolates "can we join and read the room?".
//
// Build + run (from the plugin root; R = your RACK_DIR):
//   c++ -std=c++11 -I src -I "$R/dep/include" \
//     test/njclient_test.cpp src/net/ninjam/NjClient.cpp src/net/ninjam/NjProtocol.cpp \
//     "$R/dep/lib/libcrypto.a" \
//     -o build/njclient_test
//   build/njclient_test <host> [port] [seconds] [user] [password]
//
// Defaults to port 2049. With no password it logs in anonymously; pass a password
// to log in as a registered user. Creds are command-line only (never in source).
// Use a private test server, or a public one (ninbot/ninjamer) — mind etiquette.

#include "../src/net/ninjam/NjClient.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

using namespace akaudio::nj;

int main(int argc, char** argv) {
	if (argc < 2) {
		std::printf("usage: %s <host> [port] [seconds] [user]\n", argv[0]);
		return 2;
	}
	std::string host = argv[1];
	int port = argc > 2 ? std::atoi(argv[2]) : 2049;
	double seconds = argc > 3 ? std::atof(argv[3]) : 20.0;
	std::string user = argc > 4 ? argv[4] : "akaudio-test";
	std::string pass = argc > 5 ? argv[5] : "";
	bool tx = argc > 6 && std::string(argv[6]) == "tx"; // transmit a test tone on channel 0

	std::printf("Connecting to %s:%d as %s%s for %.0f s%s\n\n",
	            host.c_str(), port, pass.empty() ? "anonymous:" : "", user.c_str(), seconds,
	            tx ? "  [TRANSMITTING 330 Hz tone on channel \"akaudio-tx\"]" : "");

	NjClient client;
	NjClient::Callbacks cb;
	cb.onState = [](NjClient::State s, const std::string& msg) {
		std::printf("[state] %s%s%s\n", stateName(s),
		            msg.empty() ? "" : " - ", msg.c_str());
	};
	cb.onLog = [](const std::string& m) { std::printf("[log]   %s\n", m.c_str()); };
	cb.onConfig = [](int bpm, int bpi) {
		std::printf("[tempo] %d BPM, %d BPI  (interval ~%.1f s)\n",
		            bpm, bpi, bpm > 0 ? bpi * 60.0 / bpm : 0.0);
	};
	cb.onUserInfo = [](const std::vector<UserChannel>& users) {
		std::printf("[roster] %zu channel update(s):\n", users.size());
		for (const auto& u : users) {
			std::printf("    %-20s ch%-2d %-16s %s vol=%+.1fdB pan=%d\n",
			            u.user.c_str(), u.channelIdx, ("\"" + u.channel + "\"").c_str(),
			            u.active ? "[active]" : "[gone]  ",
			            u.volumeDb10 / 10.0, u.pan);
		}
	};

	const double sr = 48000.0;
	client.setSampleRate(sr);
	if (tx)
		client.setTransmit({"akaudio-tx"}, 0.5f); // one local channel, q0.5 (~190 kbps)
	client.start(host, port, user, pass, cb);
	float txPhase = 0.f;

	// Consume the decoded mix at ~realtime (10 ms blocks), like the audio thread would,
	// so the mixer's ring backpressure behaves correctly. Track peak/RMS + underruns.
	const int blockMs = 10;
	const int blockFrames = (int)(sr * blockMs / 1000.0);
	double peak = 0.0, sumSq = 0.0;
	long long heard = 0, underruns = 0;
	double nextReport = 2.0, elapsed = 0.0;

	auto start = std::chrono::steady_clock::now();
	auto deadline = start +
	                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
	                    std::chrono::duration<double>(seconds));
	while (std::chrono::steady_clock::now() < deadline && client.isRunning()) {
		for (int i = 0; i < blockFrames; i++) {
			if (tx) {
				float s = 0.3f * std::sin(2.f * 3.14159265f * txPhase);
				txPhase += 330.f / (float) sr;
				if (txPhase >= 1.f) txPhase -= 1.f;
				client.captureFrame(0, s, s);
			}
			float l = 0.f, r = 0.f;
			if (client.pull(l, r)) {
				double a = std::fabs((double)l), b = std::fabs((double)r);
				if (a > peak) peak = a;
				if (b > peak) peak = b;
				sumSq += (double)l * l + (double)r * r;
				heard++;
			} else {
				underruns++;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(blockMs));
		// Real wall-clock elapsed (not the sum of sleeps, which lags by the per-iteration
		// pull work and makes the run look like it drops early).
		elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		if (elapsed >= nextReport) {
			nextReport += 2.0;
			std::printf("[audio] t=%4.1fs intervals=%ld errors=%ld missed=%ld peak=%.3f heardFrames=%lld underruns=%lld\n",
			            elapsed, client.intervalsDecoded(), client.decodeErrors(),
			            client.missedIntervals(), peak, heard, underruns);
		}
	}

	std::printf("\nStopping...\n");
	client.stop();
	double rms = heard > 0 ? std::sqrt(sumSq / (2.0 * heard)) : 0.0;
	std::printf("Final state: %s\n", stateName(client.state()));
	std::printf("Audio: intervals=%ld errors=%ld peak=%.4f rms=%.4f heardFrames=%lld underruns=%lld\n",
	            client.intervalsDecoded(), client.decodeErrors(), peak, rms, heard, underruns);
	return 0;
}
