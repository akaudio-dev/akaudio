// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

// Offline test for NjArchive (the NINJAM wire archive). Feeds synthetic RX/TX interval
// bytes, then checks the files, the JSON-lines index, and the stats. No Rack link.
//   c++ -std=c++11 -I src test/archive_test.cpp src/net/ninjam/NjArchive.cpp \
//       src/net/Log.cpp -lpthread -o build/archive_test && build/archive_test <tmpdir>
#include "net/ninjam/NjArchive.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace akaudio::nj;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static bool fileExists(const std::string& p) { std::ifstream f(p, std::ios::binary); return (bool) f; }
static std::string readAll(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
static int countLines(const std::string& s) { int n = 0; for (char c : s) if (c == '\n') n++; return n; }

int main(int argc, char** argv) {
	std::string dir = (argc > 1) ? argv[1] : "/tmp/akaudio_archive_test";
	NjArchive a;
	a.start(dir, /*recordTx=*/true);
	CHECK(a.running(), "archive running after start");
	CHECK(a.dir() == dir, "dir reported");

	std::vector<uint8_t> b1(300, 0xAB), b2(500, 0xCD), btx(700, 0x12);
	a.setSessionFrame(1000);
	a.archiveRx("bob", 0, b1.data(), b1.size(), 120, 8, 48000.f, 384000);
	a.setSessionFrame(2000);
	a.archiveRx("bob", 0, b2.data(), b2.size(), 120, 8, 48000.f, 384000); // second interval, same channel
	a.archiveRx("alice", 1, b1.data(), b1.size(), 120, 8, 48000.f, 384000);
	a.setSessionFrame(3000);
	a.archiveTx(0, btx.data(), btx.size(), 120, 8, 48000.f, 384000);

	// A silence interval (0 bytes) must NOT create a file.
	a.archiveRx("bob", 0, nullptr, 0, 120, 8, 48000.f, 384000);

	// Let the writer thread drain.
	for (int i = 0; i < 200 && a.totalIntervals() < 4; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	CHECK(a.totalIntervals() == 4, "4 intervals written (got %ld)", a.totalIntervals());
	CHECK(a.totalBytes() == (long)(b1.size() + b2.size() + b1.size() + btx.size()), "total bytes (got %ld)", a.totalBytes());

	// Files: sequential ids across both players and tx.
	CHECK(fileExists(dir + "/players/000000_bob_ch0.ogg"), "first rx file");
	CHECK(fileExists(dir + "/players/000001_bob_ch0.ogg"), "second rx file (same channel, new seq)");
	CHECK(fileExists(dir + "/players/000002_alice_ch1.ogg"), "alice file");
	CHECK(fileExists(dir + "/tx/000003_mix.ogg"), "tx file");
	CHECK(!fileExists(dir + "/players/000004_bob_ch0.ogg"), "silence interval wrote no file");

	// Raw bytes are copied verbatim.
	std::string f0 = readAll(dir + "/players/000000_bob_ch0.ogg");
	CHECK(f0.size() == b1.size() && (unsigned char) f0[0] == 0xAB, "rx bytes verbatim");
	std::string ftx = readAll(dir + "/tx/000003_mix.ogg");
	CHECK(ftx.size() == btx.size() && (unsigned char) ftx[0] == 0x12, "tx bytes verbatim");

	// Index: one JSONL line per interval, carrying the session frame + fields.
	std::string idx = readAll(dir + "/index.jsonl");
	CHECK(countLines(idx) == 4, "4 index lines (got %d)", countLines(idx));
	CHECK(idx.find("\"user\":\"bob\"") != std::string::npos, "index has user bob");
	CHECK(idx.find("\"sessionFrame\":2000") != std::string::npos, "index has the 2000 stamp");
	CHECK(idx.find("\"tx\":true") != std::string::npos, "index marks the tx interval");
	CHECK(idx.find("\"file\":\"players/000002_alice_ch1.ogg\"") != std::string::npos, "index file path");

	// Stats snapshot.
	auto st = a.status();
	CHECK(st.size() == 3, "3 sources in stats (got %zu)", st.size()); // bob/ch0, alice/ch1, you(tx)
	long bobIv = 0; for (auto& p : st) if (p.label.find("ch0") != std::string::npos && !p.tx) bobIv = p.intervals;
	CHECK(bobIv == 2, "bob ch0 has 2 intervals (got %ld)", bobIv);

	// recordTx off => tx intervals dropped.
	a.stop();
	a.start(dir + "2", /*recordTx=*/false);
	a.archiveTx(0, btx.data(), btx.size(), 120, 8, 48000.f, 384000);
	a.archiveRx("bob", 0, b1.data(), b1.size(), 120, 8, 48000.f, 384000);
	for (int i = 0; i < 100 && a.totalIntervals() < 1; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	CHECK(a.totalIntervals() == 1, "recordTx off: only the rx interval written (got %ld)", a.totalIntervals());
	a.stop();

	std::printf("%s (%d failures)\n", fails ? "FAIL" : "PASS: NjArchive", fails);
	return fails ? 1 : 0;
}
