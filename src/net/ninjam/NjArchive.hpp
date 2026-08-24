// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
// NjArchive — the NINJAM "wire archive" (docs/LOOPER_DESIGN.md §4 item 3, §7). Saves
// every interval that crossed the connection as the RAW OGG bytes it already is: each
// received (user, channel) interval and each of our transmitted mix intervals, one
// `.ogg` file apiece, plus a JSON-lines index stamping each on the session timeline.
// Zero re-encode, zero decode — the bytes are copied straight to disk.
//
// Threading: producers are the NINJAM net thread (received intervals) and the TX
// thread (our uploads) — NOT the audio thread; a brief mutex to enqueue a copy is
// fine. A single writer thread drains the queue and does the file I/O. status() is a
// mutex-guarded snapshot for the UI thread. Rack-free (paths passed in).
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace akaudio {
namespace nj {

class NjArchive {
public:
	struct PlayerStat {
		std::string label;   // "user / ch0", or "you (tx)"
		long intervals = 0;
		long bytes = 0;
		bool tx = false;
	};

	NjArchive() = default;
	~NjArchive();
	NjArchive(const NjArchive&) = delete;
	NjArchive& operator=(const NjArchive&) = delete;

	// UI/setup thread. start() creates <dir>/{players,tx}/ + index.jsonl and launches
	// the writer thread; a second start() while running is a no-op (same dir) or a
	// stop+restart (different dir). stop() flushes + joins.
	void start(const std::string& sessionDir, bool recordTx);
	void stop();
	bool running() const { return run_.load(std::memory_order_acquire); }
	// By-value is deliberate: dir_ is reassigned by a restart, so a caller-held
	// reference could dangle.
	// cppcheck-suppress returnByReference
	std::string dir() const;
	long totalIntervals() const { return nIntervals.load(std::memory_order_relaxed); }
	long totalBytes() const { return nBytes.load(std::memory_order_relaxed); }

	// Audio thread: publish the current session-timeline position (cheap atomic). Each
	// interval is stamped with the value current when it is enqueued.
	void setSessionFrame(uint64_t sf) { sessionFrame.store(sf, std::memory_order_relaxed); }

	// Net thread: a complete received interval's raw OGG bytes (no-op unless running).
	void archiveRx(const std::string& user, int chidx, const uint8_t* bytes, size_t len,
	               int bpm, int bpi, float sampleRate, int frames);
	// TX thread: a complete transmitted-mix interval's raw OGG bytes (no-op unless
	// running or recordTx is off).
	void archiveTx(int chidx, const uint8_t* bytes, size_t len,
	               int bpm, int bpi, float sampleRate, int frames);

	// UI thread: snapshot of per-source counts, most-recent-first-ish (insertion order).
	std::vector<PlayerStat> status() const;

private:
	struct Job {
		bool tx = false;
		std::string user;
		int chidx = 0;
		std::vector<uint8_t> bytes;
		uint64_t sessionFrame = 0;
		int bpm = 0, bpi = 0, frames = 0;
		float sampleRate = 0.f;
	};
	void run();                       // writer thread
	void enqueue(Job&& j);
	void bumpStat(const std::string& key, const std::string& label, bool tx, long bytes);

	std::string dir_;
	std::atomic<bool> recordTx_{false};
	std::atomic<bool> run_{false};
	std::atomic<bool> abort_{false};
	std::atomic<uint64_t> sessionFrame{0};
	std::atomic<long> nIntervals{0};
	std::atomic<long> nBytes{0};
	long seq = 0;                     // writer thread only: global interval sequence

	std::thread thread;
	std::mutex qmu;
	std::deque<Job> queue;            // guarded by qmu (multi-producer, single-consumer)

	mutable std::mutex smu;           // guards stats
	std::vector<PlayerStat> stats;    // insertion order (writer thread appends via bumpStat)
	std::vector<std::string> statKeys;
};

} // namespace nj
} // namespace akaudio
