// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
// Session — the Looper's on-disk grid (docs/LOOPER_DESIGN.md §5.4 / §10 / §11, M4).
// Implements LooperSink: the LooperWorker hands it each committed take to encode as a
// raw OGG file (t<track>_s<slot>.ogg) and index in session.json; an overwritten or
// cleared take's live file is renamed into history/, never deleted. So the grid survives
// a reload and a whole jam can later be reassembled in a DAW.
//
// Rack-free and self-contained (portable file I/O + the vendored OGG-Vorbis encoder), so
// it links into test/session_test.cpp with no Rack. Thread model: save()/clear()/flush()
// run on the LooperWorker thread (heavy: OGG encode + file writes); setDir()/setRoom()/
// setTrackName()/setSlotSettings() run on the UI thread. A mutex guards the shared model;
// the encode itself runs unlocked (it only reads the caller's PCM).
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "LooperEngine.hpp"

namespace akaudio {
namespace looper {

class Session : public LooperSink {
public:
	Session() = default;
	~Session() override = default;
	Session(const Session&) = delete;
	Session& operator=(const Session&) = delete;

	// ---- UI thread ----
	// Point the session at its `.../looper` directory. Changing it starts a fresh
	// manifest (a new jam); directories are created lazily on the first real write, so an
	// empty session leaves nothing on disk. "" disables writing.
	void setDir(const std::string& looperDir);
	// Carry the whole session into a new folder (the adoption move): live take files
	// are byte-copied old→new and the manifest rewritten there, rows kept as-is —
	// including takes whose grid doesn't match the live one (they grey + re-derive on
	// load, exactly like a restore; a RAM re-save can't do this, and derived tiles
	// have no files of their own — the originals are what travel). The old folder
	// keeps everything; events continue in the new folder's log.
	// `retireSource`: after the worker's byte-copies all verify, delete the source's
	// live files + manifest + events log and remove its (then-empty) folders — move
	// semantics for a same-run own `_session` folder that would otherwise linger as a
	// full duplicate. history/ (if any) is never touched, which simply keeps the
	// folder alive. Any copy failure cancels the retirement.
	void migrateTo(const std::string& newLooperDir, bool retireSource = false);
	void setRoom(const std::string& room);
	void setTrackName(int track, const std::string& name);
	// Reflect a late repeats/decay/follow edit into the manifest. Works for take-less
	// cells too (an empty "rest" step in a follow chain gets a settings-only row).
	void setSlotSettings(int track, int slot, int repeats, float decayDb, int followSlot);
	// A session restored from disk counts as written: its session.json already exists,
	// so later settings/name edits (and clears) may rewrite it. Without this, flush()
	// and clear() refuse until a brand-new take lands — a reloaded patch would never
	// persist edits, and clearing a restored slot would orphan its manifest row.
	void markRestored();
	std::string dir() const;
	bool hasWritten() const; // true once at least one take reached disk this session
	// The live cell's on-disk name, "t<t>_s<s>.ogg" — the naming contract SessionMirror
	// syncs by (history/ files carry a stamp prefix and never match it). Inline so
	// SessionMirror links without Session.cpp's encoder dependencies.
	static std::string liveName(int t, int s) {
		char b[32];
		(void) std::snprintf(b, sizeof(b), "t%d_s%d.ogg", t, s);
		return std::string(b);
	}

	// ---- Clip loader ----
	// UI thread: queue a saved take to restore (its OGG at `absPath`).
	void enqueueLoad(int track, int slot, const std::string& absPath, const TakeMeta& meta);
	// UI thread: seed the manifest model with a take already on disk (a restored slot), so a
	// later save/clear rewrites session.json with the restored takes preserved, not dropped.
	void noteExistingTake(int track, int slot, const std::string& file, const TakeMeta& meta);

	// ---- Worker thread (LooperSink) ----
	void save(int track, int slot, const float* pcm, const TakeMeta& meta) override;
	void clear(int track, int slot) override;
	void flush() override;
	bool nextLoad(int& track, int& slot, std::vector<float>& pcm, int& frames, TakeMeta& meta) override;
	// Performance event → one appended line in <dir>/events.jsonl (the as-played
	// timeline, docs §12). Events arriving before setDir buffer (bounded) and flush to
	// the folder once it is known.
	void event(const LoopEvent& ev) override;

	// Encode quality (VBR), archive-grade by default (~250 kbps). UI thread.
	void setQuality(float q) { quality_ = q; }

private:
	struct Rec {
		bool present = false;
		std::string file;
		int frames = 0, bpm = 0, bpi = 0;
		float sampleRate = 0.f, peak = 0.f;
		uint64_t startFrame = 0;
		int repeats = 0;
		float decayDb = 0.f;
		int followSlot = 0; // 0 = stop, 1..8 = launch slot N after done
		std::string created;
	};

	void writeManifestLocked();
	void appendEventLocked(const LoopEvent& ev); // mu_ held; dir_ non-empty
	void flushPendingEventsLocked();             // mu_ held

	struct LoadReq { int track, slot; std::string path; TakeMeta meta; };
	std::mutex loadMu_;
	std::vector<LoadReq> loadQueue_;

	mutable std::mutex mu_;
	std::string dir_;
	std::string room_;
	std::string created_;                        // session start stamp (set on first write)
	std::string names_[MAX_TRACKS];
	Rec recs_[MAX_TRACKS][MAX_SLOTS];
	int sBpm_ = 0, sBpi_ = 0, sFrames_ = 0;      // session-level grid (from the latest save)
	float sSampleRate_ = 0.f;
	bool dirty_ = false;
	bool everWrote_ = false;
	int serial_ = 1;                             // OGG stream serial (varies per file)
	long histSeq_ = 0;                           // uniquifies history filenames within a second
	float quality_ = 0.8f;
	std::vector<LoopEvent> pendingEvents_;       // events before setDir (bounded, drop-oldest)
	// Migration byte-copies deferred to the worker (drained by flush(), skip-if-exists).
	std::vector<std::pair<std::string, std::string>> pendingCopies_;
	// Move-semantics retirement: files to delete + the dir to remove once the copies
	// above all verified (cleared on any copy failure).
	std::vector<std::string> retireFiles_;
	std::string retireDir_;
};

} // namespace looper
} // namespace akaudio
