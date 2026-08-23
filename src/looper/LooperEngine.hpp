// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
// LooperEngine — the audio-thread half of the Looper (docs/LOOPER_DESIGN.md §5):
// tracks × slots of one-interval takes, always-record into a rolling buffer, and the
// arm → commit-at-the-boundary state machine (capture / launch / stop / overdub,
// scenes, per-slot repeats + decay). Rack-free and driven one frame at a time by a
// ClockFrame (Ninjam's JamClockMessage in the plugin, a synthetic clock in test/).
//
// THE REALTIME CONTRACT (CLAUDE.md) applies to everything called from tick():
// no allocation, no locks, no I/O. Buffers are allocated/freed ONLY by LooperWorker;
// the audio thread asks for them (Cmd) and receives them (Reply) over SPSC queues and
// commits by pointer rotation. A committed take is immutable; overdub builds a new
// buffer (staging) and swaps it in at the boundary.
//
// Capture has Ableton clip semantics: pressing an empty slot arms it; at the next
// boundary it starts RECORDING (the interval is recorded into the rolling buffer as
// always); at the boundary after that the completed interval becomes its take and it
// starts playing, replacing the slot that was playing. Pressing again cancels.
//
// Buffer accounting per track: `rec` (recording), `last` (the interval just
// completed — what a RECORDING slot takes), `spare` (pre-fetched replacement); per
// filled slot one take; per in-flight overdub one staging buffer.
#include <atomic>
#include <cstdint>

#include "Spsc.hpp"

namespace akaudio {
namespace looper {

static const int MAX_TRACKS = 8;
static const int MAX_SLOTS = 8;
static const int THUMB_BINS = 64;
static const float GATE = 0.000316f; // −70 dBFS on the ±1 scale

// A stereo-interleaved buffer of `frames` frames. Allocated/freed by the worker only.
struct Buf {
	float* pcm;
	int frames;
};

enum SlotState { EMPTY = 0, FILLED = 1, PLAYING = 2, RECORDING = 3 };
enum Pending { NONE = 0, CAPTURE = 1, LAUNCH = 2, STOP = 3, OVERDUB = 4 };

// One frame of clock, as the engine needs it (a strict subset of JamClockMessage).
struct ClockFrame {
	bool running;
	int intervalFrames;   // N
	int frameInInterval;  // 0..N-1
	bool downbeat;        // frameInInterval == 0 this frame
	uint32_t gridGeneration;
	uint64_t sessionFrame;
	float sampleRate;
	int bpm, bpi;
};

// One frame of input for one track.
struct TrackIn {
	bool present; // a cable feeds this track (recorded while present)
	bool tx;      // live input goes to the mix ("on air"); loops always do
	float l, r;   // ±1
};

// Audio → worker.
struct Cmd {
	enum Kind { ALLOC, OVERDUB_COPY, RELEASE } kind;
	int track, slot, frames, upto;
	uint32_t seq;
	Buf* a; // OVERDUB_COPY: the take to copy; RELEASE: the buffer
	Buf* b; // OVERDUB_COPY: the rolling buffer whose [0, upto) is added
};
// Worker → audio.
struct Reply {
	enum Kind { ALLOC, OVERDUB_COPY } kind;
	int track, slot;
	uint32_t seq;
	Buf* buf;
};
// UI → audio.
struct Intent {
	enum Kind { CLEAR } kind;
	int track, slot;
};

struct Take {
	Buf* buf = nullptr;
	int frames = 0;
	int bpm = 0, bpi = 0;
	float sampleRate = 0.f;
	uint64_t startFrame = 0; // session timeline position of the captured interval
	float peak = 0.f;
};

struct Slot {
	// UI-visible (atomics; the UI never touches the audio-only fields).
	std::atomic<int> state{EMPTY};
	std::atomic<int> pending{NONE};
	std::atomic<int> repeats{0};       // 0 = ∞ (UI writes, audio reads)
	std::atomic<float> decayDb{0.f};   // dB per repetition, 0…−6
	std::atomic<int> repCount{0};
	std::atomic<float> gain{1.f};
	std::atomic<bool> playable{true};  // take length matches the live grid
	std::atomic<double> flashAt{-1.0}; // wall time of a refused action (UI: red flash)
	float thumb[THUMB_BINS] = {};      // display only; written at a boundary
	// Audio-thread only.
	Take take;
	bool startedThisBoundary = false;
	Buf* staging = nullptr; // overdub in progress: take + this interval's input
	uint32_t odSeq = 0;
	int odCatch = 0;        // frames of the rolling buffer already added into staging
	bool odReady = false;
};

struct Track {
	Slot slots[MAX_SLOTS];
	std::atomic<int> playingSlot{-1};
	std::atomic<bool> present{false};
	std::atomic<int> bufs{0};        // rolling buffers held (rec + last + spare) — diagnostics
	float live[THUMB_BINS] = {};     // interval in progress (audio writes; an armed slot draws it)
	float lastLive[THUMB_BINS] = {}; // the completed interval (capture thumbnail)
	// Audio-thread only.
	Buf* rec = nullptr;
	Buf* last = nullptr;
	Buf* spare = nullptr;
	bool recPending = false;
	float peak = 0.f, lastPeak = 0.f;
	float txGain = 1.f;   // smoothed TX latch (no click)
	float gateEnv = 0.f;  // live-thru gate envelope
};

class LooperWorker;

class LooperEngine {
public:
	LooperEngine();
	~LooperEngine();
	LooperEngine(const LooperEngine&) = delete;
	LooperEngine& operator=(const LooperEngine&) = delete;

	// Worker thread lifecycle (UI/setup thread).
	void start();
	void stop();

	// ---- Audio thread ----
	// One frame: commits on c.downbeat, records `in`, plays the loops, mixes.
	// `now` = wall-clock seconds for the UI's refusal flash (any monotonic clock).
	// MIX = on-air tracks' live-thru + all loops; CUE = private (TX-off) tracks' live-thru
	// (monitor what you're trying without the room hearing). Both through the safety limiter.
	void tick(const ClockFrame& c, const TrackIn* in, int nTracks, double now,
	          float& outL, float& outR, float& cueL, float& cueR);
	// Button intents (from param edges, audio thread).
	void pressSlot(int t, int s, bool overdubMode);
	void pressScene(int row);
	void stopTrack(int t);
	void stopAll();

	// ---- UI thread ----
	void requestClear(int t, int s);
	int pendingCount() const;
	Slot& slotAt(int idx) { return tracks[idx / MAX_SLOTS].slots[idx % MAX_SLOTS]; }

	Track tracks[MAX_TRACKS];
	std::atomic<int> defRepeats{0};      // defaults for new captures
	std::atomic<float> defDecayDb{0.f};
	std::atomic<int> intervalFrames{0};  // current N (UI)
	std::atomic<long> allocations{0};    // buffers allocated by the worker (diagnostics/test)

private:
	friend class LooperWorker;
	void regrid(const ClockFrame& c);
	void boundary(const ClockFrame& c, double now);
	void drainReplies();
	void drainIntents();
	void requestRec(int t);
	void release(Buf* b);
	void setPlaying(Track& tr, int s);
	void refuse(Slot& sl, double now) { sl.flashAt.store(now, std::memory_order_relaxed); }
	void dropOverdub(Slot& sl);

	SpscQueue<Cmd, 256> cmds;      // audio → worker
	SpscQueue<Reply, 256> replies; // worker → audio
	SpscQueue<Intent, 64> intents; // UI → audio
	LooperWorker* worker = nullptr;

	int N = 0;
	float sr = 0.f;
	uint32_t gen = 0;
	bool haveGrid = false;
	float gateDecay = 0.f;
	uint32_t seqCounter = 1;
	int lastFrameRecorded = -1; // frames [0, lastFrameRecorded] of the interval are in `rec`
};

} // namespace looper
} // namespace akaudio
