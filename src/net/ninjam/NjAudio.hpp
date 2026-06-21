#pragma once
// NjAudio — NINJAM interval audio: reassembly, OGG decode, mix, realtime pacing.
//
// NINJAM streams audio as tempo-aligned intervals. Each (user, channel) uploads one
// self-contained OGG Vorbis interval per interval-period; the server relays it to us as
// DOWNLOAD_INTERVAL_BEGIN (announce guid+codec) + DOWNLOAD_INTERVAL_WRITE chunks (last
// flagged). A zero guid = a silence interval (keeps the per-channel cadence).
//
// Threading (v1 "pre-mix to the ring", mirrors StreamClient's pull() contract):
//   - Net thread (NjClient::run) feeds beginInterval()/writeInterval() and the channel
//     roster. On the final chunk we decode (stb_vorbis) + resample the whole interval to
//     exactly intervalSamples frames and enqueue it on that channel.
//   - Mix thread (owned here): at each interval boundary pops one ready interval per
//     channel, mixes with per-channel volume/pan, and pushes frames to the ring. Ring
//     backpressure paces the whole thing at realtime and yields the natural 1-interval
//     cadence. The inherent one-interval latency gives the decoder its lead time.
//   - Audio thread: pull() one stereo frame (lock-free), exactly like Radio/Stream.
//
// v1 limits (see docs/ninjam-client-plan.md §4): OGG Vorbis only (no Opus); linear
// resample; channels that miss a boundary play silence that interval (no per-user
// phase-lock — that is the deferred v2 NinjamController-style model).
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../RingBuffer.hpp"

namespace akaudio {
namespace nj {

class NjAudio {
public:
	NjAudio() = default;
	~NjAudio();

	NjAudio(const NjAudio&) = delete;
	NjAudio& operator=(const NjAudio&) = delete;

	// Engine sample rate (UI/any thread).
	void setSampleRate(double sr);
	// Tempo from CONFIG_CHANGE; recomputes intervalSamples (clears queued audio on change).
	void setTempo(int bpm, int bpi);

	// Roster record from USERINFO_CHANGE (net thread).
	void onUserChannel(const std::string& user, int chidx, bool active, int volDb10, int pan);

	// Interval transfer (net thread).
	void beginInterval(const std::string& user, int chidx, const uint8_t guid[16], uint32_t fourcc);
	void writeInterval(const uint8_t guid[16], const uint8_t* data, size_t len, bool last);

	void start(); // launch the mix thread (idempotent)
	void stop();  // stop the mix thread + clear state

	// Audio thread: next stereo frame; false on underrun (output silence).
	bool pull(float& l, float& r) { return ring.pull(l, r); }

	// Diagnostics (harness / panel).
	long intervalsDecoded() const { return nDecoded.load(std::memory_order_relaxed); }
	long decodeErrors() const { return nErrors.load(std::memory_order_relaxed); }
	int activeChannels() const { return nActive.load(std::memory_order_relaxed); }

private:
	struct Channel {
		bool active = false;
		float gainL = 1.f, gainR = 1.f;
		std::deque<std::vector<float>> ready; // each: intervalSamples*2 interleaved, or empty = silence
	};
	struct Transfer {
		std::string chanKey;
		uint32_t fourcc = 0;
		bool ogg = false;
		std::vector<uint8_t> bytes;
	};

	static std::string chanKey(const std::string& user, int chidx);
	void recomputeInterval();
	void enqueue(const std::string& key, std::vector<float>&& interval); // locks
	// Decode a complete OGG interval -> stereo interleaved of `frames` length. Empty on failure.
	std::vector<float> decodeOgg(const uint8_t* data, size_t len, int frames);
	void mixLoop();

	StereoRingBuffer ring{1 << 16};
	std::thread mixThread;
	std::atomic<bool> running{false};
	std::atomic<bool> abort{false};

	std::atomic<double> sampleRate{48000.0};
	std::atomic<int> intervalSamples{0};
	int bpm = 0, bpi = 0;

	std::mutex mu; // guards channels (+ tempo fields for recompute)
	std::map<std::string, Channel> channels;

	std::map<std::string, Transfer> transfers; // net thread only (keyed by 16-byte guid)

	std::atomic<long> nDecoded{0};
	std::atomic<long> nErrors{0};
	std::atomic<int> nActive{0};

	static const size_t kMaxReady = 4; // cap per-channel backlog (drop oldest)
};

} // namespace nj
} // namespace akaudio
