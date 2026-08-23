// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

// Offline test for looper::Session (docs/LOOPER_DESIGN.md §10 / §13, M4): hand it
// committed takes and check the OGG files, the session.json manifest, the history/
// retire on overwrite + clear, and atomicity (no leftover .tmp). No Rack link — Session
// is Rack-free (portable file I/O + the vendored OGG-Vorbis encoder).
//   see the `unittest` target in the Makefile for the exact build line.
#include "looper/Session.hpp"

#include <cmath>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <string>
#include <vector>

using namespace akaudio::looper;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static bool fileExists(const std::string& p) { std::ifstream f(p, std::ios::binary); return (bool) f; }
static std::string readAll(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
static bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// Names of the entries in `dir` (files + subdirs, no . / ..); empty if it doesn't exist.
static std::vector<std::string> listDir(const std::string& dir) {
	std::vector<std::string> out;
	DIR* d = opendir(dir.c_str());
	if (!d) return out;
	for (struct dirent* e = readdir(d); e; e = readdir(d)) {
		std::string n = e->d_name;
		if (n != "." && n != "..") out.push_back(n);
	}
	closedir(d);
	return out;
}
static int countEndingWith(const std::vector<std::string>& v, const std::string& suffix) {
	int n = 0;
	for (const auto& s : v)
		if (s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0) n++;
	return n;
}
static int countEndingWith(const std::string& dir, const std::string& suffix) { return countEndingWith(listDir(dir), suffix); }

static TakeMeta meta(int frames, uint64_t startFrame, float peak, int repeats, float decayDb) {
	TakeMeta m;
	m.frames = frames; m.sampleRate = 48000.f; m.bpm = 120; m.bpi = 4;
	m.startFrame = startFrame; m.peak = peak; m.repeats = repeats; m.decayDb = decayDb;
	return m;
}

int main(int argc, char** argv) {
	std::string base = (argc > 1) ? argv[1] : "/tmp/akaudio_session_test";
	std::string dir = base + "/looper";

	const int N = 4800; // 0.1 s @ 48k
	std::vector<float> pcm((size_t) N * 2);
	for (int f = 0; f < N; f++) {
		float x = 0.4f * std::sin(2.f * 3.14159265f * 220.f * f / 48000.f);
		pcm[(size_t) f * 2] = x;
		pcm[(size_t) f * 2 + 1] = -x;
	}

	Session s;
	s.setDir(dir);
	s.setRoom("testroom");
	s.setTrackName(0, "piano");

	// --- Save a take ---
	s.save(0, 0, pcm.data(), meta(N, 9600, 0.71f, 2, -3.f));

	std::string live = dir + "/t0_s0.ogg";
	CHECK(fileExists(live), "take file written");
	std::string bytes = readAll(live);
	CHECK(bytes.size() > 100, "take file non-trivial (%zu bytes)", bytes.size());
	CHECK(bytes.size() >= 4 && bytes.compare(0, 4, "OggS") == 0, "take file is a real OGG stream");
	CHECK(countEndingWith(dir, ".tmp") == 0, "no leftover .tmp after atomic write");

	std::string man = readAll(dir + "/session.json");
	CHECK(!man.empty(), "session.json written");
	CHECK(contains(man, "\"file\": \"t0_s0.ogg\""), "manifest lists the take file");
	CHECK(contains(man, "\"room\": \"testroom\""), "manifest carries the room");
	CHECK(contains(man, "\"name\": \"piano\""), "manifest carries the track name");
	CHECK(contains(man, "\"repeats\": 2"), "manifest carries repeats");
	CHECK(contains(man, "\"bpm\": 120"), "manifest carries bpm");
	CHECK(contains(man, "\"startFrame\": 9600"), "manifest carries the session-timeline start");

	// --- Clip loader: the saved OGG decodes back to real audio (round-trip) ---
	{
		s.enqueueLoad(0, 0, live, meta(N, 9600, 0.71f, 2, -3.f));
		int t = -1, sl = -1, frames = 0;
		std::vector<float> back;
		TakeMeta gm{};
		CHECK(s.nextLoad(t, sl, back, frames, gm), "nextLoad returned the queued take");
		CHECK(t == 0 && sl == 0, "load targets the right slot (%d,%d)", t, sl);
		CHECK(frames == N, "declared take length preserved (%d)", frames);
		CHECK(gm.repeats == 2 && gm.startFrame == 9600, "load carries the take metadata");
		int dn = (int) (back.size() / 2);
		CHECK(dn > N - 2048 && dn < N + 2048, "decoded ~N frames (%d vs %d)", dn, N);
		double rms = 0;
		int m = dn < N ? dn : N;
		for (int f = 0; f < m; f++) rms += (double) back[(size_t) f * 2] * back[(size_t) f * 2];
		rms = m ? std::sqrt(rms / m) : 0;
		// input was a 0.4-amplitude sine (RMS ~0.283); Vorbis preserves it to within a hair.
		CHECK(rms > 0.2 && rms < 0.35, "decoded take is real audio at the right level (rms %.3f)", rms);
		int t2, s2, fr2;
		std::vector<float> more;
		TakeMeta m2{};
		CHECK(!s.nextLoad(t2, s2, more, fr2, m2), "load queue empty after the one take");
	}

	// --- Late settings edit reflects into the manifest ---
	s.setSlotSettings(0, 0, 4, -6.f);
	s.flush();
	man = readAll(dir + "/session.json");
	CHECK(contains(man, "\"repeats\": 4"), "manifest updated after a settings edit");

	// --- Overwrite: the old take is retired into history/, a new live file lands ---
	s.save(0, 0, pcm.data(), meta(N, 19200, 0.5f, 0, 0.f));
	CHECK(fileExists(live), "live take still present after overwrite");
	CHECK(countEndingWith(dir + "/history", "_t0_s0.ogg") == 1, "one retired copy in history/ after overwrite");
	man = readAll(dir + "/session.json");
	CHECK(contains(man, "\"startFrame\": 19200"), "manifest reflects the overwriting take");

	// --- A second track/slot, then clear the first ---
	s.setTrackName(1, "bass");
	s.save(1, 3, pcm.data(), meta(N, 0, 0.6f, 0, 0.f));
	CHECK(fileExists(dir + "/t1_s3.ogg"), "second slot written to its own file");

	s.clear(0, 0);
	CHECK(!fileExists(live), "cleared slot's live file removed");
	CHECK(countEndingWith(dir + "/history", "_t0_s0.ogg") == 2, "cleared take also retired into history/");
	man = readAll(dir + "/session.json");
	CHECK(!contains(man, "\"file\": \"t0_s0.ogg\""), "manifest no longer lists the cleared slot");
	CHECK(contains(man, "\"file\": \"t1_s3.ogg\""), "manifest still lists the surviving slot");

	// --- An untouched session writes nothing (no empty folders) ---
	Session empty;
	empty.setDir(base + "/empty/looper");
	empty.setTrackName(0, "ghost");
	empty.flush();
	CHECK(!fileExists(base + "/empty/looper/session.json"), "no manifest for a session with no takes");

	std::printf("%s (%d failures)\n", fails ? "FAIL" : "PASS: Session", fails);
	return fails ? 1 : 0;
}
