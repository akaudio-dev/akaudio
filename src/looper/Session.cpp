// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "Session.hpp"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <sys/stat.h>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef _WIN32
#include <direct.h>
#endif

#include "../net/ninjam/NjEncoder.hpp"
#include "../dep/stb_vorbis_impl.hpp"

namespace akaudio {
namespace looper {

// ---- Portable file helpers (Rack-free) -------------------------------------------

static bool pathExists(const std::string& p) {
	struct stat st;
	return stat(p.c_str(), &st) == 0;
}

static void removeDirIfEmpty(const std::string& p) {
#ifdef _WIN32
	_rmdir(p.c_str());
#else
	rmdir(p.c_str());
#endif
}

static void makeDir(const std::string& p) {
#ifdef _WIN32
	_mkdir(p.c_str());
#else
	mkdir(p.c_str(), 0755);
#endif
}

// mkdir -p: create every missing parent of `p`.
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

// Write bytes to `path.tmp` then rename over `path`, so a reader never sees a partial
// file (rename is atomic within a filesystem).
static bool writeAtomic(const std::string& path, const uint8_t* data, size_t n) {
	std::string tmp = path + ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f) return false;
		if (n) f.write((const char*) data, (std::streamsize) n);
		if (!f) { f.close(); (void) std::remove(tmp.c_str()); return false; }
	}
#ifdef _WIN32
	std::remove(path.c_str()); // Windows rename won't overwrite (POSIX does)
#endif
	if (std::rename(tmp.c_str(), path.c_str()) != 0) {
		(void) std::remove(tmp.c_str());  // best-effort cleanup
		return false;
	}
	return true;
}

static std::string nowStamp(const char* fmt) {
	// Reentrant local time: this runs on the looper's worker thread while the UI
	// thread may be stamping its own session folders (std::localtime shares one
	// static buffer process-wide).
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

// Minimal JSON string escaping (quotes, backslash, control chars → \uXXXX).
static std::string jesc(const std::string& s) {
	std::string o;
	o.reserve(s.size() + 2);
	for (unsigned char c : s) {
		switch (c) {
			case '"':  o += "\\\""; break;
			case '\\': o += "\\\\"; break;
			case '\n': o += "\\n";  break;
			case '\r': o += "\\r";  break;
			case '\t': o += "\\t";  break;
			default:
				if (c < 0x20) {
					char b[8];
					(void) std::snprintf(b, sizeof(b), "\\u%04x", c);
					o += b;
				} else {
					o += (char) c;
				}
		}
	}
	return o;
}

// ---- UI thread -------------------------------------------------------------------

void Session::setDir(const std::string& looperDir) {
	std::lock_guard<std::mutex> lk(mu_);
	if (looperDir == dir_) return;
	dir_ = looperDir;
	// New folder ⇒ fresh jam: forget the previous grid's files (names carry over — they
	// describe the instruments, not the takes). No I/O here; the first save creates it.
	for (int t = 0; t < MAX_TRACKS; t++)
		for (int s = 0; s < MAX_SLOTS; s++)
			recs_[t][s] = Rec();
	created_.clear();
	everWrote_ = false;
	dirty_ = false;
	flushPendingEventsLocked();
}

// Byte-copy a file (atomic at the destination). False if the source can't be read.
static bool copyFileBytes(const std::string& from, const std::string& to) {
	std::ifstream f(from, std::ios::binary);
	if (!f) return false;
	std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (bytes.empty()) return false;
	return writeAtomic(to, (const uint8_t*) bytes.data(), bytes.size());
}

void Session::migrateTo(const std::string& newLooperDir, bool retireSource) {
	std::lock_guard<std::mutex> lk(mu_);
	if (newLooperDir.empty() || newLooperDir == dir_) return;
	std::string oldDir = dir_;
	dir_ = newLooperDir;
	created_ = nowStamp("%Y-%m-%dT%H:%M:%S"); // a new jam starts now, not at the old stamp
	bool rows = false;
	makeDirs(dir_);
	for (int t = 0; t < MAX_TRACKS; t++) {
		for (int s = 0; s < MAX_SLOTS; s++) {
			Rec& r = recs_[t][s];
			if (!r.present && r.repeats == 0 && r.decayDb == 0.f && r.followSlot == 0)
				continue;
			rows = true;
			// Defer the byte-copies to the worker (drained by flush()): a full grid is
			// tens of MB and this runs on the UI thread at the arm moment.
			if (r.present && !r.file.empty() && !oldDir.empty()) {
				pendingCopies_.push_back({oldDir + "/" + r.file, dir_ + "/" + r.file});
				if (retireSource)
					retireFiles_.push_back(oldDir + "/" + r.file);
			}
		}
	}
	if (retireSource && !oldDir.empty()) {
		retireDir_ = oldDir;
		retireFiles_.push_back(oldDir + "/session.json");
		retireFiles_.push_back(oldDir + "/events.jsonl");
	}
	if (rows) {
		everWrote_ = true; // the new folder has a real manifest from the first moment
		writeManifestLocked();
		dirty_ = false;
	}
	flushPendingEventsLocked();
}

// mu_ held. Buffered pre-dir events land in the (now known) folder's log.
void Session::flushPendingEventsLocked() {
	if (dir_.empty() || pendingEvents_.empty()) return;
	for (const LoopEvent& ev : pendingEvents_)
		appendEventLocked(ev);
	pendingEvents_.clear();
}

void Session::setRoom(const std::string& room) {
	std::lock_guard<std::mutex> lk(mu_);
	if (room == room_) return;
	room_ = room;
	dirty_ = true;
}

void Session::setTrackName(int track, const std::string& name) {
	if (track < 0 || track >= MAX_TRACKS) return;
	std::lock_guard<std::mutex> lk(mu_);
	if (names_[track] == name) return;
	names_[track] = name;
	dirty_ = true;
}

void Session::setSlotSettings(int track, int slot, int repeats, float decayDb, int followSlot) {
	if (track < 0 || track >= MAX_TRACKS || slot < 0 || slot >= MAX_SLOTS) return;
	std::lock_guard<std::mutex> lk(mu_);
	Rec& r = recs_[track][slot];
	if (r.repeats == repeats && r.decayDb == decayDb && r.followSlot == followSlot)
		return;
	// Settings are kept even for a cell with no take (an empty "rest" step in a follow
	// chain) — the manifest writes a settings-only row for it (no file).
	r.repeats = repeats;
	r.decayDb = decayDb;
	r.followSlot = followSlot;
	dirty_ = true;
}

void Session::markRestored() {
	std::lock_guard<std::mutex> lk(mu_);
	everWrote_ = true;
}

std::string Session::dir() const {
	std::lock_guard<std::mutex> lk(mu_);
	return dir_;
}

bool Session::hasWritten() const {
	std::lock_guard<std::mutex> lk(mu_);
	return everWrote_;
}

// ---- Worker thread ---------------------------------------------------------------

void Session::save(int track, int slot, const float* pcm, const TakeMeta& meta) {
	if (track < 0 || track >= MAX_TRACKS || slot < 0 || slot >= MAX_SLOTS) return;
	if (!pcm || meta.frames <= 0 || meta.sampleRate <= 0.f) return;

	std::string outDir, name = liveName(track, slot);
	int serial;
	{
		std::lock_guard<std::mutex> lk(mu_);
		outDir = dir_;
		serial = serial_++;
	}
	if (outDir.empty()) return;

	// Encode outside the lock — a full interval is ~100 ms of CPU, and UI-thread setters
	// must not wait on it. `pcm` is the caller's immutable take buffer.
	std::vector<uint8_t> ogg = nj::encodeOggInterval(pcm, meta.frames, 2,
		(int) meta.sampleRate, quality_, serial);
	if (ogg.empty()) return; // encoder refused the params — nothing to write

	makeDirs(outDir);
	std::string livePath = outDir + "/" + name;
	// An existing take at this slot is retired into history/ before the new one lands.
	if (pathExists(livePath)) {
		makeDirs(outDir + "/history");
		std::string hist = outDir + "/history/" + nowStamp("%Y%m%d-%H%M%S") + "_"
			+ std::to_string(histSeq_++) + "_" + name;
		(void) std::rename(livePath.c_str(), hist.c_str());  // best-effort retire
	}
	if (!writeAtomic(livePath, ogg.data(), ogg.size()))
		return;

	{
		std::lock_guard<std::mutex> lk(mu_);
		// The session may have MIGRATED to a new folder while we encoded (adoption at
		// the arm boundary — the exact moment takes commit): the file above landed in
		// the OLD folder, but the manifest below writes to the new dir_. Heal by
		// copying the fresh file into the current folder so row and file agree.
		if (dir_ != outDir && !dir_.empty()) {
			makeDirs(dir_);
			(void) copyFileBytes(livePath, dir_ + "/" + name);
		}
		if (created_.empty()) created_ = nowStamp("%Y-%m-%dT%H:%M:%S");
		Rec& r = recs_[track][slot];
		r.present = true;
		r.file = name;
		r.frames = meta.frames;
		r.bpm = meta.bpm; r.bpi = meta.bpi;
		r.sampleRate = meta.sampleRate;
		r.peak = meta.peak;
		r.startFrame = meta.startFrame;
		r.repeats = meta.repeats;
		r.decayDb = meta.decayDb;
		r.followSlot = meta.followSlot;
		if (r.created.empty()) r.created = nowStamp("%Y-%m-%dT%H:%M:%S");
		sBpm_ = meta.bpm; sBpi_ = meta.bpi; sFrames_ = meta.frames; sSampleRate_ = meta.sampleRate;
		everWrote_ = true;
		writeManifestLocked();
		dirty_ = false;
	}
}

void Session::clear(int track, int slot) {
	if (track < 0 || track >= MAX_TRACKS || slot < 0 || slot >= MAX_SLOTS) return;
	std::string outDir, name = liveName(track, slot);
	{
		std::lock_guard<std::mutex> lk(mu_);
		outDir = dir_;
	}
	if (outDir.empty()) return;
	std::string livePath = outDir + "/" + name;
	if (pathExists(livePath)) {
		makeDirs(outDir + "/history");
		std::string hist = outDir + "/history/" + nowStamp("%Y%m%d-%H%M%S") + "_"
			+ std::to_string(histSeq_++) + "_" + name;
		(void) std::rename(livePath.c_str(), hist.c_str());  // best-effort retire
	}
	std::lock_guard<std::mutex> lk(mu_);
	recs_[track][slot] = Rec();
	if (everWrote_)
		writeManifestLocked();
	dirty_ = false;
}

void Session::flush() {
	// Deferred migration copies first (worker thread; I/O outside the lock). A cell
	// re-recorded since the migration already has a fresh file at the destination —
	// never clobber it with the old bytes.
	std::vector<std::pair<std::string, std::string>> copies;
	std::vector<std::string> retire;
	std::string retireDir;
	{
		std::lock_guard<std::mutex> lk(mu_);
		copies.swap(pendingCopies_);
		retire.swap(retireFiles_);
		retireDir.swap(retireDir_);
	}
	bool copiesOk = true;
	for (const auto& c : copies) {
		if (pathExists(c.second)) continue; // fresher file already there: never clobber
		if (!pathExists(c.first)) continue; // ghost row (file long gone): nothing to lose
		if (!copyFileBytes(c.first, c.second))
			copiesOk = false;               // a REAL copy failed: retirement is cancelled
	}
	// Move semantics: only once every copy verified does the duplicate source retire.
	// rmdir is best-effort — history/ or stray files simply keep the folder alive.
	if (!retireDir.empty()) {
		if (copiesOk) {
			for (const auto& f : retire)
				(void) std::remove(f.c_str());
			removeDirIfEmpty(retireDir);                       // .../<jam>/looper
			size_t sl = retireDir.find_last_of('/');
			if (sl != std::string::npos)
				removeDirIfEmpty(retireDir.substr(0, sl));     // .../<jam>
		}
	}
	std::lock_guard<std::mutex> lk(mu_);
	if (!dirty_ || !everWrote_ || dir_.empty()) return;
	writeManifestLocked();
	dirty_ = false;
}

// Performance event → events.jsonl (the as-played timeline). Worker thread; events are
// human-rate (a handful per interval boundary), so open-append-close per line is fine.
void Session::event(const LoopEvent& ev) {
	std::lock_guard<std::mutex> lk(mu_);
	if (dir_.empty()) {
		// Folder not resolved yet (a restored session's first frames): hold on to a
		// bounded backlog, oldest dropped — the log is best-effort, never a leak.
		if (pendingEvents_.size() >= 256)
			pendingEvents_.erase(pendingEvents_.begin());
		pendingEvents_.push_back(ev);
		return;
	}
	appendEventLocked(ev);
}

void Session::appendEventLocked(const LoopEvent& ev) {
	static const char* reasons[] = {"launch", "capture", "follow", "replaced", "stop",
	                                "steal", "clear", "regrid", "exhausted", "carry"};
	const char* why = ev.reason < sizeof(reasons) / sizeof(reasons[0]) ? reasons[ev.reason] : "?";
	char line[256];
	int n = std::snprintf(line, sizeof(line),
		"{\"ev\":\"%s\",\"t\":%d,\"s\":%d,\"sf\":%llu,\"take\":%llu,\"rest\":%s,"
		"\"bpm\":%d,\"bpi\":%d,\"sr\":%ld,\"gen\":%lu,\"reason\":\"%s\"}\n",
		ev.start ? "start" : "stop", ev.track, ev.slot,
		(unsigned long long) ev.sessionFrame, (unsigned long long) ev.takeStartFrame,
		ev.rest ? "true" : "false", ev.bpm, ev.bpi, (long) ev.sampleRate,
		(unsigned long) ev.gridGeneration, why);
	if (n <= 0 || n >= (int) sizeof(line)) return;
	std::string path = dir_ + "/events.jsonl";
	FILE* f = std::fopen(path.c_str(), "ab");
	if (!f) { // folder not there yet: create it once, then retry
		makeDirs(dir_);
		f = std::fopen(path.c_str(), "ab");
		if (!f) return;
	}
	(void) std::fwrite(line, 1, (size_t) n, f);
	(void) std::fclose(f);
}

// ---- Clip loader ----------------------------------------------------------------

void Session::enqueueLoad(int track, int slot, const std::string& absPath, const TakeMeta& meta) {
	std::lock_guard<std::mutex> lk(loadMu_);
	loadQueue_.push_back({track, slot, absPath, meta});
}

void Session::noteExistingTake(int track, int slot, const std::string& file, const TakeMeta& meta) {
	if (track < 0 || track >= MAX_TRACKS || slot < 0 || slot >= MAX_SLOTS) return;
	std::lock_guard<std::mutex> lk(mu_);
	Rec& r = recs_[track][slot];
	r.present = true;
	r.file = file;
	r.frames = meta.frames;
	r.bpm = meta.bpm;
	r.bpi = meta.bpi;
	r.sampleRate = meta.sampleRate;
	r.peak = meta.peak;
	r.startFrame = meta.startFrame;
	r.repeats = meta.repeats;
	r.decayDb = meta.decayDb;
	r.followSlot = meta.followSlot;
	sBpm_ = meta.bpm; sBpi_ = meta.bpi; sFrames_ = meta.frames; sSampleRate_ = meta.sampleRate;
	// The on-disk session.json is already correct — don't set everWrote_/dirty_ here; a
	// later real save/clear rewrites the manifest with these entries preserved.
}

// Worker thread: pop one queued take, decode its OGG (stb_vorbis) into interleaved stereo
// float. `frames` is set to the take's DECLARED length (meta.frames) so the buffer matches
// the interval grid exactly — Vorbis padding makes the decoded count differ by a few
// samples; the worker fits the decoded PCM into that and zero-pads any tail. Returns false
// only when the queue is empty; a missing/undecodable file returns true with pcm empty
// (the worker then skips it).
bool Session::nextLoad(int& track, int& slot, std::vector<float>& pcm, int& frames, TakeMeta& meta) {
	LoadReq req;
	{
		std::lock_guard<std::mutex> lk(loadMu_);
		if (loadQueue_.empty()) return false;
		req = loadQueue_.front();
		loadQueue_.erase(loadQueue_.begin());
	}
	track = req.track;
	slot = req.slot;
	meta = req.meta;
	frames = req.meta.frames;
	pcm.clear();

	std::ifstream f(req.path, std::ios::binary);
	if (!f) return true;
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (bytes.empty()) return true;

	int err = 0;
	stb_vorbis* v = stb_vorbis_open_memory(bytes.data(), (int) bytes.size(), &err, nullptr);
	if (!v) return true;
	stb_vorbis_info info = stb_vorbis_get_info(v);
	int nch = info.channels;
	if (nch < 1 || nch > 2) { stb_vorbis_close(v); return true; }

	std::vector<float> raw;
	const int CHUNK = 4096;
	std::vector<float> tmp((size_t) CHUNK * nch);
	for (;;) {
		int got = stb_vorbis_get_samples_float_interleaved(v, nch, tmp.data(), CHUNK * nch);
		if (got <= 0) break;
		raw.insert(raw.end(), tmp.begin(), tmp.begin() + (size_t) got * nch);
	}
	stb_vorbis_close(v);

	long n = (long) (raw.size() / (size_t) nch);
	pcm.resize((size_t) n * 2);
	for (long i = 0; i < n; i++) {
		float l = raw[(size_t) i * nch];
		float r = nch >= 2 ? raw[(size_t) i * nch + 1] : l;
		pcm[(size_t) i * 2] = l;
		pcm[(size_t) i * 2 + 1] = r;
	}
	return true;
}

// Build session.json from the in-memory model and write it atomically. mu_ held.
void Session::writeManifestLocked() {
	if (dir_.empty()) return;
	makeDirs(dir_);
	std::string j;
	j.reserve(2048);
	j += "{\n";
	j += "  \"version\": 1,\n";
	j += "  \"created\": \"" + jesc(created_) + "\",\n";
	j += "  \"room\": \"" + jesc(room_) + "\",\n";
	j += "  \"bpm\": " + std::to_string(sBpm_) + ",\n";
	j += "  \"bpi\": " + std::to_string(sBpi_) + ",\n";
	j += "  \"sampleRate\": " + std::to_string((long) sSampleRate_) + ",\n";
	j += "  \"intervalFrames\": " + std::to_string(sFrames_) + ",\n";

	j += "  \"tracks\": [";
	for (int t = 0; t < MAX_TRACKS; t++) {
		j += (t ? ",\n" : "\n");
		j += "    { \"index\": " + std::to_string(t) + ", \"name\": \"" + jesc(names_[t]) + "\" }";
	}
	j += "\n  ],\n";

	j += "  \"slots\": [";
	bool first = true;
	for (int t = 0; t < MAX_TRACKS; t++) {
		for (int s = 0; s < MAX_SLOTS; s++) {
			const Rec& r = recs_[t][s];
			// A cell with no take still gets a row when its settings are non-default
			// (an empty "rest" step in a follow chain): the row has an empty "file".
			const bool hasSettings = r.repeats != 0 || r.decayDb != 0.f || r.followSlot != 0;
			if (!r.present && !hasSettings) continue;
			j += (first ? "\n" : ",\n");
			first = false;
			char peak[32];
			(void) std::snprintf(peak, sizeof(peak), "%.4f", r.peak);
			char decay[32];
			(void) std::snprintf(decay, sizeof(decay), "%.2f", r.decayDb);
			j += "    { \"track\": " + std::to_string(t)
			   + ", \"slot\": " + std::to_string(s)
			   + ", \"file\": \"" + jesc(r.file) + "\""
			   + ", \"repeats\": " + std::to_string(r.repeats)
			   + ", \"decayDb\": " + std::string(decay)
			   + ", \"follow\": " + std::to_string(r.followSlot)
			   + ", \"created\": \"" + jesc(r.created) + "\""
			   + ", \"startFrame\": " + std::to_string((long long) r.startFrame)
			   + ", \"frames\": " + std::to_string(r.frames)
			   + ", \"bpm\": " + std::to_string(r.bpm)
			   + ", \"bpi\": " + std::to_string(r.bpi)
			   + ", \"sampleRate\": " + std::to_string((long) r.sampleRate)
			   + ", \"peak\": " + std::string(peak)
			   + " }";
		}
	}
	j += (first ? "" : "\n  ");
	j += "]\n}\n";

	writeAtomic(dir_ + "/session.json", (const uint8_t*) j.data(), j.size());
}

} // namespace looper
} // namespace akaudio
