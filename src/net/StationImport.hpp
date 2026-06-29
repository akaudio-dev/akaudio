// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
#include <string>
#include <functional>
#include <cstdint>
#include <thread>
#include <mutex>
#include <atomic>

// Background "audition this stream URL" worker.
//
// Flow (off the UI thread): VERIFY the stream actually produces audio — not just
// that it connected, but that decoded frames keep flowing (a connected-but-silent
// stream, e.g. a dead HLS mount, fails here) → IDENTIFY via radio-browser (byurl)
// for a real name + favicon → FETCH the favicon (radio-browser's only, raster
// formats via ImageCache; no CDN/placeholder fallbacks). It does NOT write
// presets — the UI thread decides whether/how to save (auto for identified
// stations, name-to-save for unknown ones) so saving, dedup and rollback all live
// in one place.
//
// The UI polls generation()/result() and applies the outcome. Rack-free (paths
// passed in, std I/O) so the test harnesses that link net/ still build.

namespace akaudio {

class StationImporter {
public:
	// A snapshot of the live stream, supplied by the caller each poll: the
	// StreamClient state int (2 == Playing, 3 == Error), the producedFrames()
	// counter, and the human status text (for a precise failure reason).
	struct Probe {
		int state = 0;
		uint64_t frames = 0;
		std::string status;
	};

	struct Result {
		bool ok = false;         // the stream verified (real audio flowed)
		bool identified = false; // radio-browser knew this URL (we have a real name)
		std::string url;
		std::string name;        // real station name, or "" when unidentified
		std::string iconPath;    // cached favicon path, or "" (UI shows synth avatar)
		std::string status;      // outcome / failure reason
	};

	StationImporter() = default;
	~StationImporter();

	StationImporter(const StationImporter&) = delete;
	StationImporter& operator=(const StationImporter&) = delete;

	// Kick the audition. probe yields the live stream snapshot. cacheDir is where a
	// favicon is written. No-op if one is already running. Returns immediately.
	void start(const std::string& url, std::function<Probe()> probe, const std::string& cacheDir);

	bool running() const { return running_.load(std::memory_order_acquire); }
	// Bumped once when a run finishes (success or failure).
	unsigned generation() const { return generation_.load(std::memory_order_acquire); }
	std::string status();
	Result result();

private:
	void run();

	std::mutex mutex;
	Result result_;
	std::string status_ = "Idle";
	std::string url_, cacheDir_;
	std::function<Probe()> probe_;

	std::thread thread;
	std::atomic<bool> running_{false};
	std::atomic<unsigned> generation_{0};
};

} // namespace akaudio
