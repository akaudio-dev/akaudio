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
	void setRoom(const std::string& room);
	void setTrackName(int track, const std::string& name);
	// Reflect a late REPEATS/DECAY edit of an already-saved slot into the manifest.
	void setSlotSettings(int track, int slot, int repeats, float decayDb);
	std::string dir() const;
	bool hasWritten() const; // true once at least one take reached disk this session

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
		std::string created;
	};

	void writeManifestLocked();
	std::string liveName(int t, int s) const;   // "t<t>_s<s>.ogg"

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
};

} // namespace looper
} // namespace akaudio
