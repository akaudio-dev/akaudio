#pragma once
// NjAudio — NINJAM interval audio: reassembly, OGG decode, per-player mix, realtime pacing.
//
// NINJAM streams audio as tempo-aligned intervals. Each (user, channel) uploads one
// self-contained OGG Vorbis interval per interval-period; the server relays it to us as
// DOWNLOAD_INTERVAL_BEGIN (announce guid+codec) + DOWNLOAD_INTERVAL_WRITE chunks (last
// flagged). A zero guid = a silence interval (keeps the per-channel cadence).
//
// Output model: each remote user is mixed (their channels summed, with vol/pan) into a
// stable "slot" (0..MAX_PLAYERS-1). The mixer writes a wide frame of all slots' stereo
// into a lock-free ring; the audio thread pulls one wide frame and fans it out to the
// module's POLY (per-player) and MAIN (sum) outputs.
//
// Threading:
//   - Net thread (NjClient::run) feeds beginInterval()/writeInterval() + the roster.
//     On the final chunk we decode (stb_vorbis) + resample the interval and enqueue it.
//   - Mix thread (here): at each interval boundary pops one ready interval per channel,
//     applies vol/pan, accumulates into per-slot stereo, and pushes wide frames. Ring
//     backpressure paces to realtime; a small prebuffer margin avoids boundary races.
//   - Audio thread: pullFrame() one wide frame (lock-free).
//
// Limits: OGG Vorbis only (no Opus); linear resample; >MAX_PLAYERS users overflow off poly.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace akaudio {
namespace nj {

static const int MAX_PLAYERS = 16;       // VCV poly cable max
static const int RING_CH = MAX_PLAYERS * 2; // interleaved stereo per slot

// Lock-free SPSC ring of fixed-width float frames (RING_CH floats per frame).
class WideRing {
public:
	explicit WideRing(size_t minFrames = 1 << 16) {
		size_t cap = 1;
		while (cap < minFrames) cap <<= 1;
		mask = cap - 1;
		buf.resize(cap * RING_CH);
	}
	bool push(const float* frame) {
		const size_t w = writeIdx.load(std::memory_order_relaxed);
		const size_t next = (w + 1) & mask;
		if (next == readIdx.load(std::memory_order_acquire)) return false; // full
		std::memcpy(&buf[w * RING_CH], frame, RING_CH * sizeof(float));
		writeIdx.store(next, std::memory_order_release);
		return true;
	}
	bool pull(float* frame) {
		const size_t rd = readIdx.load(std::memory_order_relaxed);
		if (rd == writeIdx.load(std::memory_order_acquire)) return false; // empty
		std::memcpy(frame, &buf[rd * RING_CH], RING_CH * sizeof(float));
		readIdx.store((rd + 1) & mask, std::memory_order_release);
		return true;
	}
	void clear() {
		readIdx.store(0, std::memory_order_relaxed);
		writeIdx.store(0, std::memory_order_relaxed);
	}
private:
	std::vector<float> buf;
	size_t mask = 0;
	std::atomic<size_t> writeIdx{0};
	std::atomic<size_t> readIdx{0};
};

class NjAudio {
public:
	NjAudio() = default;
	~NjAudio();

	NjAudio(const NjAudio&) = delete;
	NjAudio& operator=(const NjAudio&) = delete;

	void setSampleRate(double sr);
	void setTempo(int bpm, int bpi);

	void onUserChannel(const std::string& user, int chidx, bool active, int volDb10, int pan);

	void beginInterval(const std::string& user, int chidx, const uint8_t guid[16], uint32_t fourcc);
	void writeInterval(const uint8_t guid[16], const uint8_t* data, size_t len, bool last);

	void start();
	void stop();

	// Audio thread: pull one wide frame (RING_CH interleaved-stereo-per-slot floats).
	// Returns false on underrun (out untouched). out must hold RING_CH floats.
	bool pullFrame(float* out) { return ring.pull(out); }
	// Convenience for the standalone harness: pull a frame and sum slots to a stereo master.
	bool pull(float& l, float& r) {
		float f[RING_CH];
		if (!ring.pull(f)) return false;
		l = 0.f; r = 0.f;
		for (int i = 0; i < MAX_PLAYERS; i++) { l += f[i * 2]; r += f[i * 2 + 1]; }
		return true;
	}

	// Number of poly channels currently in use (highest assigned slot + 1; 0 if none).
	int polyChannels() const { return nPoly.load(std::memory_order_relaxed); }

	// Diagnostics.
	long intervalsDecoded() const { return nDecoded.load(std::memory_order_relaxed); }
	long decodeErrors() const { return nErrors.load(std::memory_order_relaxed); }
	int activeChannels() const { return nActive.load(std::memory_order_relaxed); }
	long missedIntervals() const { return nMissed.load(std::memory_order_relaxed); }

private:
	struct Channel {
		std::string user;
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
	void enqueue(const std::string& key, std::vector<float>&& interval);
	std::vector<float> decodeOgg(const uint8_t* data, size_t len, int frames);
	void mixLoop();
	int assignSlot(const std::string& user); // call under mu; -1 if no free slot
	void refreshSlots();                      // call under mu; free slots of departed users

	WideRing ring{1 << 16};
	std::thread mixThread;
	std::atomic<bool> running{false};
	std::atomic<bool> abort{false};

	std::atomic<double> sampleRate{48000.0};
	std::atomic<int> intervalSamples{0};
	int bpm = 0, bpi = 0;

	std::mutex mu; // guards channels, slots, tempo fields
	std::map<std::string, Channel> channels;
	std::map<std::string, int> userSlot; // username -> poly slot
	bool slotUsed[MAX_PLAYERS] = {false};

	std::map<std::string, Transfer> transfers; // net thread only (keyed by 16-byte guid)

	std::atomic<long> nDecoded{0};
	std::atomic<long> nErrors{0};
	std::atomic<int> nActive{0};
	std::atomic<long> nMissed{0};
	std::atomic<int> nPoly{0};

	static const size_t kMaxReady = 4;
};

} // namespace nj
} // namespace akaudio
