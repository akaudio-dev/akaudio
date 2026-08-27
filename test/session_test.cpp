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

static TakeMeta meta(int frames, uint64_t startFrame, float peak, int repeats, float decayDb,
                     int followSlot = 0) {
	TakeMeta m;
	m.frames = frames; m.sampleRate = 48000.f; m.bpm = 120; m.bpi = 4;
	m.startFrame = startFrame; m.peak = peak; m.repeats = repeats; m.decayDb = decayDb;
	m.followSlot = followSlot;
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
	s.save(0, 0, pcm.data(), meta(N, 9600, 0.71f, 2, -3.f, 3));

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
	CHECK(contains(man, "\"follow\": 3"), "manifest carries the follow action");
	CHECK(contains(man, "\"bpm\": 120"), "manifest carries bpm");
	CHECK(contains(man, "\"startFrame\": 9600"), "manifest carries the session-timeline start");

	// --- Clip loader: the saved OGG decodes back to real audio (round-trip) ---
	{
		s.enqueueLoad(0, 0, live, meta(N, 9600, 0.71f, 2, -3.f, 3));
		int t = -1, sl = -1, frames = 0;
		std::vector<float> back;
		TakeMeta gm{};
		CHECK(s.nextLoad(t, sl, back, frames, gm), "nextLoad returned the queued take");
		CHECK(t == 0 && sl == 0, "load targets the right slot (%d,%d)", t, sl);
		CHECK(frames == N, "declared take length preserved (%d)", frames);
		CHECK(gm.repeats == 2 && gm.startFrame == 9600, "load carries the take metadata");
		CHECK(gm.followSlot == 3, "load carries the follow action");
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
	s.setSlotSettings(0, 0, 4, -6.f, 2);
	s.flush();
	man = readAll(dir + "/session.json");
	CHECK(contains(man, "\"repeats\": 4"), "manifest updated after a settings edit");
	CHECK(contains(man, "\"follow\": 2"), "manifest updated with the follow action");

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

	// --- Settings-only row: an empty "rest" cell persists its chain settings ---
	s.setSlotSettings(2, 2, 1, 0.f, 4);
	s.flush();
	man = readAll(dir + "/session.json");
	CHECK(contains(man, "\"track\": 2, \"slot\": 2, \"file\": \"\""), "rest cell row written without a file");
	CHECK(contains(man, "\"follow\": 4"), "rest cell row carries its follow action");
	s.setSlotSettings(2, 2, 0, 0.f, 0); // back to defaults → the row disappears
	s.flush();
	man = readAll(dir + "/session.json");
	CHECK(!contains(man, "\"track\": 2, \"slot\": 2"), "default settings drop the rest row");

	// --- Restored session: markRestored() lets edits and clears rewrite the manifest.
	// (Without it, flush()/clear() refuse until a new take lands: a reloaded patch would
	// never persist settings edits, and clearing a restored slot would orphan its row —
	// which comes back on the next load as a ghost.) ---
	{
		Session r;
		r.setDir(dir); // fresh model over the same on-disk session
		r.noteExistingTake(1, 3, "t1_s3.ogg", meta(N, 0, 0.6f, 0, 0.f));
		r.markRestored();
		r.setSlotSettings(1, 3, 8, 0.f, 5);
		r.flush();
		man = readAll(dir + "/session.json");
		CHECK(contains(man, "\"repeats\": 8"), "restored session: settings edit reaches the manifest");
		CHECK(contains(man, "\"follow\": 5"), "restored session: follow edit reaches the manifest");
		r.clear(1, 3);
		CHECK(!fileExists(dir + "/t1_s3.ogg"), "restored session: clear removes the live file");
		man = readAll(dir + "/session.json");
		CHECK(!contains(man, "t1_s3.ogg"), "restored session: clear rewrites the manifest (no orphan row)");
	}

	// --- An untouched session writes nothing (no empty folders) ---
	Session empty;
	empty.setDir(base + "/empty/looper");
	empty.setTrackName(0, "ghost");
	empty.flush();
	CHECK(!fileExists(base + "/empty/looper/session.json"), "no manifest for a session with no takes");

	// --- migrateTo: the adoption move — live files byte-copied, manifest carried ---
	{
		s.save(2, 5, pcm.data(), meta(N, 28800, 0.4f, 0, 0.f)); // a live take to carry
		std::string newDir = base + "/migrated/looper";
		s.migrateTo(newDir);
		s.flush(); // the byte-copies are deferred to the worker's flush pass
		CHECK(fileExists(newDir + "/t2_s5.ogg"), "live take file copied into the new folder");
		CHECK(fileExists(dir + "/t2_s5.ogg"), "original stays in the old folder");
		std::string man2 = readAll(newDir + "/session.json");
		CHECK(contains(man2, "\"file\": \"t2_s5.ogg\"") && contains(man2, "\"startFrame\": 28800"),
		      "manifest rewritten at the destination with the carried rows");
		CHECK(contains(man2, "\"file\": \"t1_s3.ogg\""),
		      "a row whose file is gone still carries (tolerated like a restore)");
		CHECK(!fileExists(newDir + "/history"), "history/ does not travel");
		s.save(3, 1, pcm.data(), meta(N, 38400, 0.3f, 0, 0.f));
		CHECK(fileExists(newDir + "/t3_s1.ogg") && !fileExists(dir + "/t3_s1.ogg"),
		      "post-migration saves land in the new folder only");

		// --- retireSource: move semantics — after the copies verify, the duplicate
		// source folder is emptied and removed (a same-run own `_session` folder).
		std::string dst = base + "/2026-01-01/0900_x/looper";
		s.migrateTo(dst, true);
		s.flush();
		CHECK(fileExists(dst + "/t2_s5.ogg") && fileExists(dst + "/t3_s1.ogg"),
		      "moved: take files at the destination");
		CHECK(!fileExists(newDir + "/t2_s5.ogg") && !fileExists(newDir + "/session.json"),
		      "moved: source files retired after verified copies");
		CHECK(listDir(newDir).empty(), "moved: empty source folder removed");
		CHECK(fileExists(dst + "/session.json"), "moved: manifest lives at the destination");
	}

	// --- Performance events → events.jsonl (append-only; pre-setDir buffering) ---
	{
		Session ev;
		LoopEvent e {};
		e.start = true; e.track = 0; e.slot = 3; e.sessionFrame = 9600;
		e.takeStartFrame = 4800; e.bpm = 120; e.bpi = 4; e.sampleRate = 48000.f;
		e.gridGeneration = 2; e.reason = LoopEvent::R_LAUNCH;
		ev.event(e); // before setDir: buffered, no file anywhere
		std::string evDir = base + "/evtest/looper";
		ev.setDir(evDir);
		std::string evPath = evDir + "/events.jsonl";
		CHECK(fileExists(evPath), "buffered pre-setDir event flushed once the folder is known");
		e.start = false; e.sessionFrame = 19200; e.reason = LoopEvent::R_STOP;
		ev.event(e);
		e.start = true; e.slot = 4; e.rest = true; e.takeStartFrame = 0;
		e.sessionFrame = 19200; e.reason = LoopEvent::R_FOLLOW;
		ev.event(e);
		std::string log = readAll(evPath);
		int lines = 0;
		for (char ch : log)
			if (ch == '\n') lines++;
		CHECK(lines == 3, "events append, never truncate (%d lines)", lines);
		CHECK(contains(log, "\"ev\":\"start\",\"t\":0,\"s\":3,\"sf\":9600,\"take\":4800,\"rest\":false")
		      && contains(log, "\"reason\":\"launch\""), "start row carries position + identity + reason");
		CHECK(contains(log, "\"ev\":\"stop\"") && contains(log, "\"sf\":19200"), "stop row present");
		CHECK(contains(log, "\"rest\":true") && contains(log, "\"reason\":\"follow\""), "rest row marked");
		CHECK(contains(log, "\"bpm\":120,\"bpi\":4,\"sr\":48000,\"gen\":2"), "grid stamped on rows");
	}

	std::printf("%s (%d failures)\n", fails ? "FAIL" : "PASS: Session", fails);
	return fails ? 1 : 0;
}
