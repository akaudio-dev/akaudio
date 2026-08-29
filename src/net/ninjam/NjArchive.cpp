// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "NjArchive.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#include <direct.h>
#define AK_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define AK_MKDIR(p) ::mkdir((p), 0755)
#endif

#include "../Log.hpp"

namespace akaudio {
namespace nj {

static void makeDir(const std::string& p) {
	if (p.empty())
		return;
	AK_MKDIR(p.c_str()); // ignore EEXIST
}

// A filename-safe slug of a username (wire strings are arbitrary UTF-8).
static std::string slug(const std::string& s) {
	std::string o;
	for (char c : s) {
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
			o += c;
		else
			o += '_';
	}
	if (o.empty())
		o = "anon";
	if (o.size() > 40)
		o.resize(40);
	return o;
}

NjArchive::~NjArchive() {
	stop();
}

void NjArchive::start(const std::string& sessionDir, bool recordTx) {
	if (run_.load(std::memory_order_acquire)) {
		if (sessionDir == dir_) {
			recordTx_.store(recordTx, std::memory_order_relaxed);
			return; // already archiving this session
		}
		stop();
	}
	dir_ = sessionDir;
	recordTx_.store(recordTx, std::memory_order_relaxed);
	// Create every path component (asset::user's base may exist; the session subdirs won't).
	for (size_t i = 1; i < dir_.size(); i++)
		if (dir_[i] == '/') makeDir(dir_.substr(0, i));
	makeDir(dir_);
	makeDir(dir_ + "/players");
	makeDir(dir_ + "/tx");
	seq = 0;
	{
		std::lock_guard<std::mutex> lk(qmu);
		queue.clear();
	}
	{
		std::lock_guard<std::mutex> lk(smu);
		stats.clear();
		statKeys.clear();
	}
	nIntervals.store(0, std::memory_order_relaxed);
	nBytes.store(0, std::memory_order_relaxed);
	abort_.store(false, std::memory_order_relaxed);
	gen_.fetch_add(1, std::memory_order_acq_rel); // a new archive generation begins
	run_.store(true, std::memory_order_release);
	thread = std::thread(&NjArchive::run, this);
	netLog("archive: started at " + dir_);
}

void NjArchive::stop() {
	if (!run_.load(std::memory_order_acquire) && !thread.joinable())
		return;
	run_.store(false, std::memory_order_release);
	abort_.store(true, std::memory_order_release);
	if (thread.joinable())
		thread.join();
}

std::string NjArchive::dir() const {
	return dir_;
}

void NjArchive::enqueue(Job&& j) {
	{
		std::lock_guard<std::mutex> lk(qmu);
		// Bound the backlog so a stalled disk can't grow memory without limit; the
		// writer keeps up easily (a few files/sec), so this only triggers on a wedged FS.
		if (queue.size() > 256) {
			netLog("archive: queue full, dropping an interval");
			return;
		}
		queue.push_back(std::move(j));
	}
}

void NjArchive::archiveRx(const std::string& user, int chidx, const uint8_t* bytes, size_t len,
                          int bpm, int bpi, float sampleRate, int frames, uint64_t atSessionFrame) {
	if (!run_.load(std::memory_order_acquire) || !bytes || len == 0)
		return;
	Job j;
	j.tx = false;
	j.user = user;
	j.chidx = chidx;
	j.bytes.assign(bytes, bytes + len);
	j.sessionFrame = stampOr(atSessionFrame);
	j.bpm = bpm; j.bpi = bpi; j.frames = frames; j.sampleRate = sampleRate;
	enqueue(std::move(j));
}

void NjArchive::archiveTx(int chidx, const uint8_t* bytes, size_t len,
                          int bpm, int bpi, float sampleRate, int frames, uint64_t atSessionFrame) {
	if (!run_.load(std::memory_order_acquire) || !recordTx_.load(std::memory_order_relaxed)
	        || !bytes || len == 0)
		return;
	Job j;
	j.tx = true;
	j.chidx = chidx;
	j.bytes.assign(bytes, bytes + len);
	j.sessionFrame = stampOr(atSessionFrame);
	j.bpm = bpm; j.bpi = bpi; j.frames = frames; j.sampleRate = sampleRate;
	enqueue(std::move(j));
}

void NjArchive::bumpStat(const std::string& key, const std::string& label, bool tx, long bytes) {
	std::lock_guard<std::mutex> lk(smu);
	for (size_t i = 0; i < statKeys.size(); i++) {
		if (statKeys[i] == key) {
			stats[i].intervals++;
			stats[i].bytes += bytes;
			return;
		}
	}
	statKeys.push_back(key);
	PlayerStat p;
	p.label = label; p.tx = tx; p.intervals = 1; p.bytes = bytes;
	stats.push_back(p);
}

void NjArchive::run() {
	// The index is a JSON-lines file (one object per interval): append-only, robust to a
	// crash mid-session, and trivially parsed by an importer. See docs §7.2.
	std::ofstream index(dir_ + "/index.jsonl", std::ios::out | std::ios::trunc | std::ios::binary);
	while (!abort_.load(std::memory_order_acquire)) {
		Job j;
		bool have = false;
		{
			std::lock_guard<std::mutex> lk(qmu);
			if (!queue.empty()) { j = std::move(queue.front()); queue.pop_front(); have = true; }
		}
		if (!have) {
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
			continue;
		}
		long id = seq++;
		char name[128];
		std::string rel;
		if (j.tx)
			(void) std::snprintf(name, sizeof(name), "tx/%06ld_mix.ogg", id);
		else
			(void) std::snprintf(name, sizeof(name), "players/%06ld_%s_ch%d.ogg", id, slug(j.user).c_str(), j.chidx);
		rel = name;
		// Atomic write: tmp then rename, so a reader never sees a half file.
		std::string full = dir_ + "/" + rel;
		std::string tmp = full + ".tmp";
		{
			std::ofstream f(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
			if (f) {
				f.write((const char*) j.bytes.data(), (std::streamsize) j.bytes.size());
				f.flush();
			}
		}
		if (std::rename(tmp.c_str(), full.c_str()) != 0)
			netLog("archive: rename failed, interval lost: " + full);
		// Index line.
		if (index) {
			char line[512];
			// Escape the username minimally for JSON (it can contain quotes/backslashes).
			std::string u;
			for (char c : j.user) {
				if (c == '"' || c == '\\') u += '\\';
				if ((unsigned char) c < 0x20) { u += ' '; continue; }
				u += c;
			}
			(void) std::snprintf(line, sizeof(line),
				"{\"seq\":%ld,\"tx\":%s,\"user\":\"%s\",\"chidx\":%d,\"file\":\"%s\","
				"\"sessionFrame\":%llu,\"bytes\":%zu,\"bpm\":%d,\"bpi\":%d,\"frames\":%d,\"sampleRate\":%g}\n",
				id, j.tx ? "true" : "false", j.tx ? "" : u.c_str(), j.chidx, rel.c_str(),
				(unsigned long long) j.sessionFrame, j.bytes.size(), j.bpm, j.bpi, j.frames, (double) j.sampleRate);
			index.write(line, (std::streamsize) std::strlen(line));
			index.flush();
		}
		nIntervals.fetch_add(1, std::memory_order_relaxed);
		nBytes.fetch_add((long) j.bytes.size(), std::memory_order_relaxed);
		if (j.tx)
			bumpStat("\x01tx", "you (tx)", true, (long) j.bytes.size());
		else
			bumpStat(j.user + "\n" + std::to_string(j.chidx),
			         slug(j.user) + " / ch" + std::to_string(j.chidx), false, (long) j.bytes.size());
	}
}

std::vector<NjArchive::PlayerStat> NjArchive::status() const {
	std::lock_guard<std::mutex> lk(smu);
	return stats;
}

} // namespace nj
} // namespace akaudio
