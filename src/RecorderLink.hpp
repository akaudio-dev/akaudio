// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
// RecorderLink — the interface the Recorder module uses to drive Ninjam's wire archive
// (docs/LOOPER_DESIGN.md §7). Ninjam implements it; the Recorder finds it by
// dynamic_cast on its adjacent module (same plugin .dylib, so RTTI works). All calls
// are UI-thread. This keeps the two modules decoupled — the Recorder is a pure panel,
// Ninjam owns the archive and the NINJAM session.
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

namespace akaudio {

// Default place for recorded jams: ~/Music/jams (discoverable, unlike Rack's user dir).
inline std::string homeDir() {
	// getenv is only mt-unsafe against a concurrent setenv; nothing here or in Rack
	// mutates the environment.
#ifdef _WIN32
	const char* h = std::getenv("USERPROFILE");  // NOLINT(concurrency-mt-unsafe)
#else
	const char* h = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe)
#endif
	return std::string(h && *h ? h : "");
}
inline std::string defaultJamsDir() { return homeDir() + "/Music/jams"; }

// Store paths under the home dir as "~/..." so a shared patch never carries the real
// user name; a path outside home is stored verbatim.
inline std::string collapseHome(const std::string& p) {
	std::string h = homeDir();
	if (!h.empty() && p.compare(0, h.size(), h) == 0)
		return "~" + p.substr(h.size());
	return p;
}
inline std::string expandHome(const std::string& p) {
	if (!p.empty() && p[0] == '~')
		return homeDir() + p.substr(1);
	return p;
}

// Format the current local time. std::localtime shares one static buffer across all
// threads — the UI thread and the looper's disk worker both stamp session folders, so
// use the reentrant variants.
inline std::string timeStamp(const char* fmt) {
	std::time_t t = std::time(nullptr);
	std::tm tmv{};
#ifdef _WIN32
	localtime_s(&tmv, &t);
#else
	localtime_r(&t, &tmv);
#endif
	char buf[64];
	if (!std::strftime(buf, sizeof(buf), fmt, &tmv))
		(void) std::snprintf(buf, sizeof(buf), "%lld", (long long) t);
	return std::string(buf);
}

struct RecStatusRow {
	std::string label;   // "user / ch0" or "you (tx)"
	long intervals = 0;
	long bytes = 0;
	bool tx = false;
};

struct RecorderLink {
	virtual ~RecorderLink() {}
	virtual bool recArmed() const = 0;         // the Recorder's REC latch (owned here)
	virtual void setRecArmed(bool) = 0;
	virtual bool recordOwnTx() const = 0;      // also archive our transmitted mix
	virtual void setRecordOwnTx(bool) = 0;
	virtual bool recActive() const = 0;        // the archive is actually writing (armed + joined)
	virtual bool recJoined() const = 0;        // Ninjam is in a JOIN session
	virtual std::string recSessionName() const = 0; // folder name (or "" when idle)
	virtual long recIntervals() const = 0;
	virtual std::vector<RecStatusRow> recStatus() const = 0;
	virtual std::string sessionBase() const = 0;         // where new session folders are created
	virtual void setSessionBase(const std::string&) = 0;
	virtual long recBytes() const = 0;                   // total bytes written this session
};

} // namespace akaudio
