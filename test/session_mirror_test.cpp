// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

// Offline check of looper/SessionMirror: live cells + manifest sync between two
// folders, incremental (an unchanged grid = 0 writes), mtime-preserving (a hydrated
// folder mirrors back as a no-op), stale-cell removal, history/ untouched, and the
// no-manifest guard. No Rack, no encoder — just files.
//
// Build:
//   c++ -std=c++11 -I src test/session_mirror_test.cpp src/looper/SessionMirror.cpp \
//     -o build/session_mirror_test && build/session_mirror_test build/mirror_test_out

#include "looper/SessionMirror.hpp"
#include "looper/Session.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

using akaudio::looper::Session;
using akaudio::looper::clearSessionMirror;
using akaudio::looper::mirrorSession;

static int failures = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { std::cerr << "FAIL: " << msg << " (" << #cond << ")\n"; failures++; } \
} while (0)

static void makeDirs(const std::string& p) {
	std::string acc;
	for (size_t i = 0; i <= p.size(); i++) {
		if (i == p.size() || p[i] == '/' || p[i] == '\\') {
			if (acc.size() > 1)
#ifdef _WIN32
				_mkdir(acc.c_str());
#else
				mkdir(acc.c_str(), 0755);
#endif
		}
		if (i < p.size()) acc += p[i];
	}
}

static void writeFile(const std::string& p, const std::string& content) {
	std::ofstream f(p, std::ios::binary | std::ios::trunc);
	f << content;
}

static std::string readFile(const std::string& p) {
	std::ifstream f(p, std::ios::binary);
	if (!f) return "";
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static bool exists(const std::string& p) {
	struct stat st;
	return stat(p.c_str(), &st) == 0;
}

int main(int argc, char** argv) {
	std::string out = argc > 1 ? argv[1] : "build/mirror_test_out";
	std::string src = out + "/src", dst = out + "/dst", hyd = out + "/hyd";
	// Fresh start (best-effort cleanup of a previous run's files AND dirs — case 1
	// asserts dst does not exist).
	for (const std::string& d : {src, dst, hyd}) {
		clearSessionMirror(d);
		(void) std::remove((d + "/history/old.ogg").c_str());
#ifdef _WIN32
		(void) _rmdir((d + "/history").c_str()); (void) _rmdir(d.c_str());
#else
		(void) rmdir((d + "/history").c_str()); (void) rmdir(d.c_str());
#endif
	}

	// 1. No manifest in src → -1, dst never created.
	makeDirs(src);
	CHECK(mirrorSession(src, dst) == -1, "no-manifest guard");
	CHECK(!exists(dst), "dst not created without a source manifest");

	// 2. First mirror: two cells + manifest travel; history/ does not.
	makeDirs(src + "/history");
	writeFile(src + "/" + Session::liveName(0, 0), "cell-0-0 v1");
	writeFile(src + "/" + Session::liveName(2, 5), "cell-2-5 v1");
	writeFile(src + "/history/old.ogg", "retired take");
	writeFile(src + "/session.json", "{ \"v\": 1 }");
	CHECK(mirrorSession(src, dst) == 3, "first mirror copies 2 cells + manifest");
	CHECK(readFile(dst + "/" + Session::liveName(0, 0)) == "cell-0-0 v1", "cell 0,0 content");
	CHECK(readFile(dst + "/" + Session::liveName(2, 5)) == "cell-2-5 v1", "cell 2,5 content");
	CHECK(readFile(dst + "/session.json") == "{ \"v\": 1 }", "manifest content");
	CHECK(!exists(dst + "/history"), "history never travels");

	// 3. Unchanged grid → zero writes.
	CHECK(mirrorSession(src, dst) == 0, "unchanged grid is a no-op");

	// 4. One overwritten take → exactly one copy.
	writeFile(src + "/" + Session::liveName(0, 0), "cell-0-0 v2 (longer)");
	CHECK(mirrorSession(src, dst) == 1, "one changed cell = one write");
	CHECK(readFile(dst + "/" + Session::liveName(0, 0)) == "cell-0-0 v2 (longer)", "updated content");

	// 5. A cleared cell (file retired away) + rewritten manifest → removal propagates.
	(void) std::remove((src + "/" + Session::liveName(2, 5)).c_str());
	writeFile(src + "/session.json", "{ \"v\": 2 }");
	CHECK(mirrorSession(src, dst) == 2, "removal + manifest rewrite = two changes");
	CHECK(!exists(dst + "/" + Session::liveName(2, 5)), "stale cell removed from mirror");

	// 6. Hydration round-trip: snapshot → fresh folder, then back → no writes at all
	//    (mtimes are preserved through both hops).
	CHECK(mirrorSession(dst, hyd) == 2, "hydrate copies 1 cell + manifest");
	CHECK(readFile(hyd + "/" + Session::liveName(0, 0)) == "cell-0-0 v2 (longer)", "hydrated content");
	CHECK(mirrorSession(hyd, dst) == 0, "hydrated folder mirrors back as a no-op");

	// 7. Clear: cells + manifest gone, nothing else touched.
	CHECK(clearSessionMirror(dst) == 2, "clear removes 1 cell + manifest");
	CHECK(!exists(dst + "/" + Session::liveName(0, 0)), "cleared cell gone");
	CHECK(!exists(dst + "/session.json"), "cleared manifest gone");

	if (failures == 0) std::cout << "PASS: SessionMirror (0 failures)\n";
	else std::cout << "FAIL: SessionMirror (" << failures << " failures)\n";
	return failures ? 1 : 0;
}
