// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

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
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../RingBuffer.hpp"
#include "NjEncoder.hpp"

extern "C" {
typedef struct stb_vorbis stb_vorbis; // pushdata decoder handle (voice channels)
}

namespace akaudio {
namespace nj {

static const int MAX_PLAYERS = 16;       // VCV poly cable max
static const int RING_CH = MAX_PLAYERS * 2; // interleaved stereo per slot

// The wide mix ring: one frame = RING_CH floats (per-slot stereo). Same SPSC
// implementation as the stereo stream ring (net/RingBuffer.hpp).
typedef FrameRingBuffer<RING_CH> WideRing;

class NjAudio {
public:
	static const int MAX_TX = 4; // max local broadcast channels

	NjAudio() = default;
	~NjAudio();

	NjAudio(const NjAudio&) = delete;
	NjAudio& operator=(const NjAudio&) = delete;

	void setSampleRate(double sr);
	void setTempo(int bpm, int bpi);

	// `flags` bit 2 marks a voice-chat channel (played live, not interval-aligned).
	void onUserChannel(const std::string& user, int chidx, bool active, int volDb10, int pan,
	                   uint8_t flags = 0);

	void beginInterval(const std::string& user, int chidx, const uint8_t guid[16], uint32_t fourcc,
	                   uint32_t estSize = 0);
	void writeInterval(const uint8_t guid[16], const uint8_t* data, size_t len, bool last);

	void start();
	void stop();

	// ---- Transmit (capture local audio -> streaming encode -> upload callbacks) ----
	// Set the number of local channels to broadcast + VBR quality. Allocates capture
	// buffers (call from the UI/setup thread, before start() ideally). `voice` = the
	// channels are NINJAM voice chat: rolling ~2 s upload intervals, no beat grid.
	void setTransmit(int channels, float quality, bool voice = false);
	// Audio thread: align interval capture to the room's beat grid. Called at a beat
	// boundary; the elapsed part of the current interval (beats 0..beatIndex-1 of
	// beatCount) is pre-filled with silence so uploads stay downbeat-aligned while
	// capture starts at the very next beat after the user enables TX — not up to a
	// whole interval later at the next downbeat. Lock-free (atomics only).
	void armTransmit(int beatIndex, int beatCount) {
		txArmNum.store(beatIndex, std::memory_order_relaxed);
		txArmDen.store(beatCount, std::memory_order_relaxed);
		txArmPending.store(true, std::memory_order_release);
	}
	// Audio thread: push one captured stereo frame for local channel `ch`.
	void captureFrame(int ch, float l, float r) {
		// txRings (acquire) pairs with start()'s release after building the rings, so we
		// never index a not-yet-built or partially-built vector — and the vector is never
		// resized after that first build, so txCapture[ch] is a stable pointer read.
		if (!txActive.load(std::memory_order_acquire) || ch < 0
		        || ch >= nTx.load(std::memory_order_relaxed)
		        || ch >= txRings.load(std::memory_order_acquire))
			return;
		txCapture[ch]->push(l, r);
	}
	// TX-thread upload callbacks: an interval starts (send UPLOAD_INTERVAL_BEGIN)…
	std::function<void(int chidx)> onUploadBegin;
	// …then its encoded bytes stream out as they're produced; `last` closes the
	// interval (send as UPLOAD_INTERVAL_WRITE chunks, final one flagged; may fire
	// with len 0 && last to close an interval with no residual bytes).
	std::function<void(int chidx, const uint8_t* data, size_t len, bool last)> onUploadData;

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
	// True once any room audio (preview, voice, or chained interval) has reached the
	// mix this session — the UI's join-gap countdown hides itself on this.
	bool audioStarted() const { return audioStarted_.load(std::memory_order_relaxed); }

private:
	struct Channel {
		std::string user;
		bool active = false;
		bool voice = false; // NINJAM voice-chat channel (flags bit 2): played live
		float gainL = 1.f, gainR = 1.f;
		std::deque<std::vector<float>> ready; // each: intervalSamples*2 interleaved, or empty = silence
		// Interval-mode playhead (mix thread, under mu). Playback is ARRIVAL-LOCKED:
		// the first ready interval starts (after a short jitter hold) the moment it
		// is available, and subsequent ones chain seamlessly — phase-true to the
		// sender's grid instead of waiting for an arbitrary local boundary.
		std::vector<float> cur;   // interval being played (empty + silenceLeft>0 = silence interval)
		size_t curPos = 0;        // frames of cur consumed
		int silenceLeft = 0;      // remaining frames of a silence interval
		int holdFrames = -1;      // startup jitter margin left to burn; -1 = not armed
		bool playing = false;     // mid-cadence (a chain break => re-lock to arrival)
		// First-interval live preview: until the chain locks (everStarted), a non-voice
		// channel's in-flight interval is decoded progressively (pushdata) and played
		// live from vfifo, so a fresh join hears the room within ~a second of the next
		// downbeat instead of a whole interval later. When the first chained interval
		// pops, the preview tail fades out over pfade frames (a loop-point handover:
		// the interval then replays in its proper slot). Reset on re-grid so a tempo/
		// BPI change gets the same treatment.
		bool everStarted = false; // a chained interval has (ever) started on this channel
		int pfade = 0;            // preview fade-out frames remaining (0 = no fade active)
		// Voice-mode FIFO (net thread appends decoded frames under mu; mix thread
		// consumes). vhead = frames*2 already consumed (compacted periodically).
		std::vector<float> vfifo;
		size_t vhead = 0;
		bool vstarted = false;    // small prebuffer reached; cleared when it runs dry
	};
	struct Transfer {
		std::string chanKey;
		bool ogg = false;
		bool voice = false;            // decode progressively as chunks arrive
		bool preview = false;          // ALSO decode progressively, for the join-gap preview
		std::vector<uint8_t> bytes;    // interval mode: whole interval; voice: undecoded tail
		std::vector<uint8_t> ptail;    // preview mode: undecoded pushdata tail (bytes stays whole)
		// Progressive (pushdata) decoder state — voice and preview (net thread only).
		stb_vorbis* pv = nullptr;      // stb_vorbis pushdata handle
		int pvChannels = 0, pvRate = 0;
		std::vector<float> pcm;        // decoded-but-not-yet-resampled stereo frames
		double rsPos = 0.0;            // resample phase into pcm
	};

	static std::string chanKey(const std::string& user, int chidx);
	void recomputeInterval();
	void recomputeIntervalLocked(); // caller holds mu
	void enqueue(const std::string& key, std::vector<float>&& interval);
	std::vector<float> decodeOgg(const uint8_t* data, size_t len, int frames);
	static void closeTransfer(Transfer& t); // frees the pushdata decoder, if any
	// Progressive OGG decode of arriving chunks into the channel FIFO (net thread).
	// `tail` is the undecoded-bytes buffer this stream consumes: t.bytes for voice,
	// t.ptail for the join-gap preview (whose t.bytes must stay whole for the final
	// interval decode).
	void pushdataFeed(Transfer& t, std::vector<uint8_t>& tail, const uint8_t* data, size_t len);
	void voiceDeliver(Transfer& t);         // resample t.pcm -> channel vfifo (takes mu)
	void mixLoop();
	void txLoop();                            // capture -> encode -> onUploadInterval
	int assignSlot(const std::string& user); // call under mu; -1 if no free slot
	void refreshSlots();                      // call under mu; free slots of departed users

	// Output ring depth = STANDING latency: mixLoop keeps it full (backpressure), so
	// every queued frame waits its whole depth before the audio thread plays it.
	// 1<<14 frames ≈ 340 ms @48k — plenty to ride out mix-thread scheduling hiccups
	// (it refills in 2 ms polls), without burying the low-latency voice path. The old
	// 1<<16 (~1.4 s) silently added a second-plus to every received stream.
	WideRing ring{1 << 14};
	std::thread mixThread;
	std::thread txThread;
	std::atomic<bool> running{false};
	std::atomic<bool> abort{false};

	// Transmit state.
	std::atomic<bool> txActive{false};
	std::atomic<bool> txVoice{false}; // voice-chat mode: rolling 2 s intervals, no grid
	std::atomic<int> nTx{0};
	std::atomic<float> txQuality{0.5f}; // UI writes, txLoop reads — atomic to avoid a torn read
	std::vector<std::unique_ptr<StereoRingBuffer>> txCapture; // MAX_TX rings, built once in start()
	std::atomic<int> txRings{0}; // # rings actually built; release-published by start()
	// Pending beat-grid arming request (armTransmit → txLoop): prefill the next
	// interval with N*num/den silence frames.
	std::atomic<bool> txArmPending{false};
	std::atomic<int> txArmNum{0};
	std::atomic<int> txArmDen{1};

	std::atomic<double> sampleRate{48000.0};
	std::atomic<int> intervalSamples{0};
	int bpm = 0, bpi = 0;
	// A server tempo change is applied at the next interval boundary (in mixLoop),
	// not instantly, so audio already decoded/queued for the current interval isn't
	// re-gridded mid-flight (matches njclient/JamTaba). Guarded by mu.
	int pendingBpm = 0, pendingBpi = 0;
	bool tempoPending = false;
	// A sample-rate change can arrive on the audio DEVICE thread (Rack dispatches
	// onSampleRateChange from the RtAudio callback on auto-rate), which must not take
	// `mu` or free queued intervals. setSampleRate just stores the rate + sets this;
	// mixLoop does the locked recompute/drop at the next boundary.
	std::atomic<bool> ratePending{false};

	std::mutex mu; // guards channels, slots, tempo fields
	std::map<std::string, Channel> channels;
	std::map<std::string, int> userSlot; // username -> poly slot
	bool slotUsed[MAX_PLAYERS] = {false};

	std::map<std::string, Transfer> transfers; // net thread only (keyed by 16-byte guid)

	std::atomic<bool> audioStarted_{false};
	std::atomic<long> nDecoded{0};
	std::atomic<long> nErrors{0};
	std::atomic<int> nActive{0};
	std::atomic<long> nMissed{0};
	std::atomic<int> nPoly{0};

	static const size_t kMaxReady = 4;
};

} // namespace nj
} // namespace akaudio
