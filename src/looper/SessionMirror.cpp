// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "SessionMirror.hpp"
#include "Session.hpp"

#include <cstdio>
#include <fstream>
#include <sys/stat.h>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <sys/utime.h>
#else
#include <sys/time.h>
#endif

namespace akaudio {
namespace looper {

// ---- Portable file helpers (same style as Session.cpp) ---------------------------

struct FileInfo {
	bool exists = false;
	long long size = 0;
	long long mtime = 0; // microseconds — sub-second so a same-second rewrite is seen
};

static FileInfo fileInfo(const std::string& p) {
	FileInfo fi;
	struct stat st;
	if (stat(p.c_str(), &st) == 0 && (st.st_mode & S_IFREG)) {
		fi.exists = true;
		fi.size = (long long) st.st_size;
#if defined(_WIN32)
		fi.mtime = (long long) st.st_mtime * 1000000LL;
#elif defined(__APPLE__)
		fi.mtime = (long long) st.st_mtimespec.tv_sec * 1000000LL + st.st_mtimespec.tv_nsec / 1000;
#else
		fi.mtime = (long long) st.st_mtim.tv_sec * 1000000LL + st.st_mtim.tv_nsec / 1000;
#endif
	}
	return fi;
}

static void makeDir(const std::string& p) {
#ifdef _WIN32
	_mkdir(p.c_str());
#else
	mkdir(p.c_str(), 0755);
#endif
}

static void makeDirs(const std::string& p) {
	if (p.empty()) return;
	std::string acc;
	for (size_t i = 0; i < p.size(); i++) {
		char c = p[i];
		if (c == '/' || c == '\\') {
			if (acc.size() > 1) makeDir(acc); // skip the leading "/" root
			acc += '/';
		} else {
			acc += c;
		}
	}
	makeDir(acc);
}

// Copy src → dst atomically (tmp + rename) and stamp dst with src's mtime, so a later
// mirror pass sees "unchanged" exactly and a hydrated folder mirrors back as a no-op.
static bool copyPreservingMtime(const std::string& src, const std::string& dst, long long mtime) {
	std::ifstream in(src, std::ios::binary);
	if (!in) return false;
	std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	in.close();
	std::string tmp = dst + ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out) return false;
		if (!bytes.empty()) out.write(bytes.data(), (std::streamsize) bytes.size());
		if (!out) { out.close(); (void) std::remove(tmp.c_str()); return false; }
	}
	(void) std::remove(dst.c_str()); // Windows rename won't overwrite
	if (std::rename(tmp.c_str(), dst.c_str()) != 0) {
		(void) std::remove(tmp.c_str());
		return false;
	}
	// A failed stamp just means an equal-content recopy on the next pass — correct,
	// merely wasteful (same for a destination filesystem that truncates timestamps).
#ifdef _WIN32
	struct _utimbuf ut;
	ut.actime = ut.modtime = (time_t) (mtime / 1000000LL);
	(void) _utime(dst.c_str(), &ut);
#else
	struct timeval tv[2];
	tv[0].tv_sec = tv[1].tv_sec = (time_t) (mtime / 1000000LL);
	tv[0].tv_usec = tv[1].tv_usec = (suseconds_t) (mtime % 1000000LL);
	(void) utimes(dst.c_str(), tv);
#endif
	return true;
}

// Sync one file src → dst: copy when size/mtime differ, delete dst when src is gone.
// Returns 1 if it wrote or deleted anything, 0 for a no-op.
static int syncFile(const std::string& src, const std::string& dst) {
	FileInfo s = fileInfo(src);
	FileInfo d = fileInfo(dst);
	if (!s.exists) {
		if (!d.exists) return 0;
		return std::remove(dst.c_str()) == 0 ? 1 : 0;
	}
	if (d.exists && d.size == s.size && d.mtime == s.mtime)
		return 0;
	return copyPreservingMtime(src, dst, s.mtime) ? 1 : 0;
}

// ---- Public API ------------------------------------------------------------------

int mirrorSession(const std::string& srcDir, const std::string& dstDir) {
	if (srcDir.empty() || dstDir.empty()) return -1;
	if (!fileInfo(srcDir + "/session.json").exists)
		return -1; // no live grid to mirror — leave dst alone, don't even create it
	makeDirs(dstDir);
	int changes = 0;
	for (int t = 0; t < MAX_TRACKS; t++)
		for (int s = 0; s < MAX_SLOTS; s++) {
			std::string name = Session::liveName(t, s);
			std::string sp = srcDir; sp += '/'; sp += name;
			std::string dp = dstDir; dp += '/'; dp += name;
			changes += syncFile(sp, dp);
		}
	changes += syncFile(srcDir + "/session.json", dstDir + "/session.json");
	return changes;
}

int clearSessionMirror(const std::string& dstDir) {
	if (dstDir.empty()) return 0;
	int removed = 0;
	for (int t = 0; t < MAX_TRACKS; t++)
		for (int s = 0; s < MAX_SLOTS; s++) {
			std::string p = dstDir; p += '/'; p += Session::liveName(t, s);
			if (std::remove(p.c_str()) == 0) removed++;
		}
	if (std::remove((dstDir + "/session.json").c_str()) == 0) removed++;
	return removed;
}

} // namespace looper
} // namespace akaudio
