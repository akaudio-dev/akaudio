// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
// RecorderLink — the interface the Recorder module uses to drive Ninjam's wire archive
// (docs/LOOPER_DESIGN.md §7). Ninjam implements it; the Recorder finds it by
// dynamic_cast on its adjacent module (same plugin .dylib, so RTTI works). All calls
// are UI-thread. This keeps the two modules decoupled — the Recorder is a pure panel,
// Ninjam owns the archive and the NINJAM session.
#include <cstdlib>
#include <string>
#include <vector>

namespace akaudio {

// Default place for recorded jams: ~/Music/jams (discoverable, unlike Rack's user dir).
inline std::string defaultJamsDir() {
#ifdef _WIN32
	const char* h = std::getenv("USERPROFILE");
#else
	const char* h = std::getenv("HOME");
#endif
	return std::string(h && *h ? h : ".") + "/Music/jams";
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
