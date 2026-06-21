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

	std::printf("Connecting to %s:%d as %s%s for %.0f s\n\n",
	            host.c_str(), port, pass.empty() ? "anonymous:" : "", user.c_str(), seconds);

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

	client.start(host, port, user, pass, cb);

	auto deadline = std::chrono::steady_clock::now() +
	                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
	                    std::chrono::duration<double>(seconds));
	while (std::chrono::steady_clock::now() < deadline && client.isRunning())
		std::this_thread::sleep_for(std::chrono::milliseconds(100));

	std::printf("\nStopping...\n");
	client.stop();
	std::printf("Final state: %s\n", stateName(client.state()));
	return 0;
}
