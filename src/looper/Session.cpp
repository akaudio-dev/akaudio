// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "Session.hpp"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <sys/stat.h>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#endif

#include "../net/ninjam/NjEncoder.hpp"

namespace akaudio {
namespace looper {

// ---- Portable file helpers (Rack-free) -------------------------------------------

static bool pathExists(const std::string& p) {
	struct stat st;
	return stat(p.c_str(), &st) == 0;
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
		if (!f) { f.close(); std::remove(tmp.c_str()); return false; }
	}
	if (std::rename(tmp.c_str(), path.c_str()) != 0) {
		std::remove(tmp.c_str());
		return false;
	}
	return true;
}

static std::string nowStamp(const char* fmt) {
	std::time_t t = std::time(nullptr);
	char buf[64];
	std::strftime(buf, sizeof(buf), fmt, std::localtime(&t));
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
					std::snprintf(b, sizeof(b), "\\u%04x", c);
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

void Session::setSlotSettings(int track, int slot, int repeats, float decayDb) {
	if (track < 0 || track >= MAX_TRACKS || slot < 0 || slot >= MAX_SLOTS) return;
	std::lock_guard<std::mutex> lk(mu_);
	Rec& r = recs_[track][slot];
	if (!r.present || (r.repeats == repeats && r.decayDb == decayDb)) return;
	r.repeats = repeats;
	r.decayDb = decayDb;
	dirty_ = true;
}

std::string Session::dir() const {
	std::lock_guard<std::mutex> lk(mu_);
	return dir_;
}

bool Session::hasWritten() const {
	std::lock_guard<std::mutex> lk(mu_);
	return everWrote_;
}

std::string Session::liveName(int t, int s) const {
	char b[32];
	std::snprintf(b, sizeof(b), "t%d_s%d.ogg", t, s);
	return std::string(b);
}

// ---- Worker thread ---------------------------------------------------------------

void Session::save(int track, int slot, const float* pcm, const TakeMeta& meta) {
	if (track < 0 || track >= MAX_TRACKS || slot < 0 || slot >= MAX_SLOTS) return;
	if (!pcm || meta.frames <= 0 || meta.sampleRate <= 0.f) return;

	std::string dir, name = liveName(track, slot);
	int serial;
	{
		std::lock_guard<std::mutex> lk(mu_);
		dir = dir_;
		serial = serial_++;
	}
	if (dir.empty()) return;

	// Encode outside the lock — a full interval is ~100 ms of CPU, and UI-thread setters
	// must not wait on it. `pcm` is the caller's immutable take buffer.
	std::vector<uint8_t> ogg = nj::encodeOggInterval(pcm, meta.frames, 2,
		(int) meta.sampleRate, quality_, serial);
	if (ogg.empty()) return; // encoder refused the params — nothing to write

	makeDirs(dir);
	std::string livePath = dir + "/" + name;
	// An existing take at this slot is retired into history/ before the new one lands.
	if (pathExists(livePath)) {
		makeDirs(dir + "/history");
		std::string hist = dir + "/history/" + nowStamp("%Y%m%d-%H%M%S") + "_"
			+ std::to_string(histSeq_++) + "_" + name;
		std::rename(livePath.c_str(), hist.c_str());
	}
	if (!writeAtomic(livePath, ogg.data(), ogg.size()))
		return;

	{
		std::lock_guard<std::mutex> lk(mu_);
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
		if (r.created.empty()) r.created = nowStamp("%Y-%m-%dT%H:%M:%S");
		sBpm_ = meta.bpm; sBpi_ = meta.bpi; sFrames_ = meta.frames; sSampleRate_ = meta.sampleRate;
		everWrote_ = true;
		writeManifestLocked();
		dirty_ = false;
	}
}

void Session::clear(int track, int slot) {
	if (track < 0 || track >= MAX_TRACKS || slot < 0 || slot >= MAX_SLOTS) return;
	std::string dir, name = liveName(track, slot);
	{
		std::lock_guard<std::mutex> lk(mu_);
		dir = dir_;
	}
	if (dir.empty()) return;
	std::string livePath = dir + "/" + name;
	if (pathExists(livePath)) {
		makeDirs(dir + "/history");
		std::string hist = dir + "/history/" + nowStamp("%Y%m%d-%H%M%S") + "_"
			+ std::to_string(histSeq_++) + "_" + name;
		std::rename(livePath.c_str(), hist.c_str());
	}
	std::lock_guard<std::mutex> lk(mu_);
	recs_[track][slot] = Rec();
	if (everWrote_)
		writeManifestLocked();
	dirty_ = false;
}

void Session::flush() {
	std::lock_guard<std::mutex> lk(mu_);
	if (!dirty_ || !everWrote_ || dir_.empty()) return;
	writeManifestLocked();
	dirty_ = false;
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
			if (!r.present) continue;
			j += (first ? "\n" : ",\n");
			first = false;
			char peak[32];
			std::snprintf(peak, sizeof(peak), "%.4f", r.peak);
			char decay[32];
			std::snprintf(decay, sizeof(decay), "%.2f", r.decayDb);
			j += "    { \"track\": " + std::to_string(t)
			   + ", \"slot\": " + std::to_string(s)
			   + ", \"file\": \"" + jesc(r.file) + "\""
			   + ", \"repeats\": " + std::to_string(r.repeats)
			   + ", \"decayDb\": " + std::string(decay)
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
