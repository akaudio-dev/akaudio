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
// starts playing, replacing the slot that was playing. Pressing an ARMED slot cancels
// the arm; pressing a RECORDING slot queues a FINISH — at the boundary the take
// commits and replays (a chain closes and cycles from its head); pressing again
// cancels the finish (the recording rolls on). The track STOP button is the discard.
//
// Buffer accounting per track: `rec` (recording), `last` (the interval just
// completed — what a RECORDING slot takes), `spare` (pre-fetched replacement); per
// filled slot one take; per in-flight overdub one staging buffer.
#include <atomic>
#include <cstdint>
#include <vector>

#include "Spsc.hpp"

namespace akaudio {
namespace looper {

static const int MAX_TRACKS = 8;
static const int MAX_SLOTS = 8;
static const int THUMB_BINS = 64;
static const float GATE = 0.000316f; // −70 dBFS on the ±1 scale
// Auto-advance capture ("keep playing → keep recording"): a committed take whose final
// TAIL_SECONDS stay above TAIL_GATE means the player blew through the downbeat — the
// recording rolls into the next empty slot below instead of the take starting to loop.
// TAIL_GATE is much hotter than GATE on purpose: a released chord's decay/reverb tail
// crossing the boundary must not chain (−40 dB ≈ inaudible under a live instrument).
static const float TAIL_GATE = 0.01f; // −40 dBFS
static const float TAIL_SECONDS = 0.3f;

// A stereo-interleaved buffer of `frames` frames. Allocated/freed by the worker only.
struct Buf {
	float* pcm;
	int frames;
};

enum SlotState { EMPTY = 0, FILLED = 1, PLAYING = 2, RECORDING = 3 };
enum Pending { NONE = 0, CAPTURE = 1, LAUNCH = 2, STOP = 3, OVERDUB = 4, FINISH = 5 };

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

// Metadata for a committed take, handed to the sink so it can encode + index the file
// (docs §10). All plain data — no Rack, no Buf ownership.
struct TakeMeta {
	int frames = 0;
	float sampleRate = 0.f;
	int bpm = 0, bpi = 0;
	uint64_t startFrame = 0; // session-timeline position of the take's first frame
	float peak = 0.f;
	int repeats = 0;
	float decayDb = 0.f;
	int followSlot = 0; // after done: 0 = stop, 1..8 = launch slot N (0 also = key absent in old manifests)
};

// One performance event: a grid cell audibly started or stopped playing, stamped on the
// shared session timeline (docs §12 — the "timeline as it was played" reconstruction).
// Emitted by the engine at the same commit points that flip a slot's PLAYING state and
// shipped to the sink over the worker queue; Session appends them to events.jsonl.
// `takeStartFrame` is the identity of the audio that was playing (a take's capture
// position, stable across overdubs, changed by a re-record) — the exporter only maps a
// span to a file when the manifest row still carries the same identity.
struct LoopEvent {
	enum Reason : uint8_t {
		R_LAUNCH = 0,    // explicit launch commit
		R_CAPTURE,       // capture committed and started playing
		R_FOLLOW,        // follow action jumped here
		R_REPLACED,      // another slot started on this track (monophonic)
		R_STOP,          // explicit stop commit (button / scene column)
		R_STEAL,         // a capture took over the track
		R_CLEAR,         // the playing cell was cleared (mid-interval, UI intent)
		R_REGRID,        // tempo/grid change demoted the playing cell
		R_EXHAUSTED,     // repeats/decay done, follow = stop (or refused target)
		R_CARRY,         // session re-pointed to a new folder; an already-playing cell
		                 // re-opens its span in the new session's log
	};
	bool start = false;          // true = play begins, false = play ends
	int track = 0, slot = 0;
	uint64_t sessionFrame = 0;   // when, on the shared session timeline
	uint64_t takeStartFrame = 0; // identity of the audio playing (0 for rest cells)
	bool rest = false;           // no take: a silent "rest" step in a follow chain
	int bpm = 0, bpi = 0;
	float sampleRate = 0.f;
	uint32_t gridGeneration = 0;
	uint8_t reason = R_LAUNCH;
};

// The disk side of the Looper (M4): the worker hands committed takes here to be encoded
// to OGG and indexed, and overwritten/cleared takes to be retired into history/. The
// engine calls it ONLY from the worker thread (never the audio thread), so implementations
// may allocate / do file I/O freely. `pcm` is read-only and valid for the call's duration
// (a committed take is immutable; its RELEASE is always ordered after its save). Left null
// in tests that don't exercise persistence, so the engine/worker carry no encoder link.
struct LooperSink {
	virtual ~LooperSink() {}
	virtual void save(int track, int slot, const float* pcm, const TakeMeta& meta) = 0;
	virtual void clear(int track, int slot) = 0; // retire the slot's live file into history/
	virtual void flush() = 0;                    // write the manifest if metadata changed
	// Restore (v2 clip loader): pop the next take to load and decode its OGG into `pcm`
	// (interleaved stereo, `frames` frames). Returns false when the load queue is empty.
	// Worker thread — decoding + file I/O live here, off the audio thread.
	virtual bool nextLoad(int& track, int& slot, std::vector<float>& pcm, int& frames,
	                      TakeMeta& meta) { return false; }
	// A performance event (cell started/stopped playing). Worker thread. Default no-op
	// so sinks that don't record the timeline (tests, future sinks) need no change.
	virtual void event(const LoopEvent&) {}
};

// A decoded take on its way from the worker into a slot (clip loader / BPI converter).
// The worker allocates + fills `buf` (like a committed take) and computes the thumbnail;
// the audio thread installs it as a FILLED take. Buffer lifetime follows the usual rule —
// freed by the worker when the slot is later cleared/overwritten.
struct LoadInstall {
	int track, slot;
	Buf* buf;
	TakeMeta meta;
	float thumb[THUMB_BINS];
	// Guarded install (BPI conversions): only land if the slot still holds `expect`
	// (the source take the conversion was derived from), or — expect == null — if the
	// slot is still EMPTY (the split's second half). A stale result is recycled, so a
	// clear/re-record between enqueue and install can never be clobbered.
	bool guard;
	Buf* expect;
	bool derived; // mark the installed take as tempo-derived (RAM-only; see Slot::derived)
};

// Audio → worker.
struct Cmd {
	enum Kind { ALLOC, OVERDUB_COPY, RELEASE, SAVE, CLEAR_FILE, CONVERT, EVENT } kind;
	int track, slot, frames, upto; // CONVERT: frames = new N; upto = split target slot (−1 = tile)
	uint32_t seq;
	Buf* a;      // OVERDUB_COPY: the take to copy; RELEASE: the buffer; SAVE/CONVERT: the source take
	Buf* b;      // OVERDUB_COPY: the rolling buffer whose [0, upto) is added
	TakeMeta meta; // SAVE: the take's metadata; CONVERT: new-grid bpm/bpi + the ORIGINAL settings
	LoopEvent ev;  // EVENT: the performance event (FIFO with SAVE keeps take-before-event order)
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
	enum Kind { CLEAR, CLEAR_ALL, CARRY_SPANS } kind;
	int track, slot; // CLEAR_ALL / CARRY_SPANS ignore both
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
	std::atomic<int> followSlot{0};    // after done: 0 = stop, 1..8 = launch slot N on this track
	std::atomic<int> repCount{0};
	std::atomic<float> gain{1.f};
	std::atomic<bool> playable{true};  // take length matches the live grid
	// This take is a RAM-only tempo derivation (a BPI tile or split half): the settings
	// sweep must NOT push its rewired settings into the manifest — the disk keeps the
	// original take and its original settings, and reload + regrid re-derives. Derived
	// takes are not converted again on further tempo changes (they grey out; reload
	// restores the originals). Cleared on capture/clear/real-load.
	std::atomic<bool> derived{false};
	std::atomic<bool> overdubbing{false}; // this slot is the live overdub target (UI marker)
	std::atomic<double> flashAt{-1.0}; // wall time of a refused action (UI: red flash)
	float thumb[THUMB_BINS] = {};      // display only; written at a boundary
	// Audio-thread only.
	Take take;
	int armFrame = 0; // frame-in-interval of the capture press: the pickup window start
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
	int chainFrom = -1;   // predecessor of the auto-advance chain's armed cell (−1 = no chain)
	int chainHead = -1;   // the chain's first cell — a FINISH press launches the replay here
	bool recPrevOk = false; // post-rotation `rec` still holds the immediately-previous
	                        // completed interval (pickup source), not a fresh spare
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

	// Disk persistence sink (M4). Set before start(); the worker calls it. Null = takes
	// stay in RAM only (the M1 behaviour, and what the engine tests use).
	void setSink(LooperSink* s) { sink = s; }

	// Clip loader (worker + test): hand a decoded take to the audio thread to install as a
	// FILLED slot. Buffer ownership passes to the engine. Returns false if the queue is
	// full (the caller then recycles the buffer).
	bool submitLoad(const LoadInstall& li) { return loads.push(li); }

	// Worker thread lifecycle (UI/setup thread).
	void start();
	void stop();

	// ---- Audio thread ----
	// One frame: commits on c.downbeat, records `in`, plays the loops, mixes.
	// `now` = wall-clock seconds for the UI's refusal flash (any monotonic clock).
	// MIX = on-air tracks' live-thru + all loops; CUE = private (TX-off) tracks' live-thru
	// (monitor what you're trying without the room hearing). Both through the safety limiter.
	// `trackOutLR` (optional, ≥ nTracks*2 floats): each track's own pre-limiter output
	// (loop + gated live-thru), interleaved 2t / 2t+1 — the per-channel POLY out.
	// `wantMix`: when false, skip the MIX submix + its limiter (outL/outR = 0) — nothing
	// is patched to MIX OUT, so there's no reason to compute it (recording, loops, CUE and
	// the per-track POLY out are unaffected).
	void tick(const ClockFrame& c, const TrackIn* in, int nTracks, double now,
	          float& outL, float& outR, float& cueL, float& cueR, float* trackOutLR = nullptr,
	          bool wantMix = true);
	// Button intents (from param edges, audio thread).
	void pressSlot(int t, int s, bool overdubLatch);
	void pressScene(int row);
	void stopTrack(int t);
	void stopAll();
	// End the track's rolling recording (an explicit press on the track disarms it;
	// `except` = a slot whose own press semantics handle the press — a press ON the
	// recording cell queues a FINISH instead of discarding).
	void cancelRecording(int t, int except);

	// ---- UI thread ----
	void requestClear(int t, int s);
	// Clear the whole grid as ONE intent (the per-cell queue holds only 63 — a 64-cell
	// burst would silently drop the last).
	void requestClearAll();
	// UI thread: after the session migrated to a new folder (adoption), re-open the
	// playing cells' spans in the new events log (files/rows travel via
	// Session::migrateTo — the engine only re-anchors the as-played timeline).
	void requestCarrySpans();
	int pendingCount() const;
	Slot& slotAt(int idx) { return tracks[idx / MAX_SLOTS].slots[idx % MAX_SLOTS]; }

	Track tracks[MAX_TRACKS];
	LooperSink* sink = nullptr;          // worker-thread disk writer (M4); null = RAM only
	// Continuous overdub (set each frame by the module): while the latch is on, the
	// selected playing slot overdubs every interval; changing the selection moves the
	// overdub to the newly selected cell at the next boundary.
	std::atomic<bool> overdubMode{false};   // the OVERDUB latch
	std::atomic<int> overdubSel{-1};        // selected slot index (track*MAX_SLOTS+slot), −1 none
	std::atomic<bool> declickEnabled{true}; // fade each loop cycle's ends to 0 (no click); tests disable it
	std::atomic<int> defRepeats{0};      // defaults for new captures
	std::atomic<float> defDecayDb{0.f};
	// Keep recording into the next empty slot while the player plays through the
	// downbeat (tail gate; see TAIL_GATE above). On by default; the menu toggle is the
	// escape hatch for never-silent sources (drones/pads), where the chain would
	// otherwise eat the whole column.
	std::atomic<bool> autoAdvance{true};
	// A BPM change re-pitches takes (varispeed, tape-style) within 0.5×–2×. Off = takes
	// grey out on tempo changes instead (re-record rather than shift pitch).
	std::atomic<bool> repitch{true};
	std::atomic<int> intervalFrames{0};  // current N (UI)
	std::atomic<long> allocations{0};    // buffers allocated by the worker (diagnostics/test)

private:
	friend class LooperWorker;
	void regrid(const ClockFrame& c);
	void boundary(const ClockFrame& c, double now);
	void drainReplies();
	void drainIntents();
	void requestRec(int t);
	void drainLoads();             // install decoded takes from the worker (clip loader)
	bool armOverdub(int t, int s); // request staging = copy(take) for the next interval's overdub
	void saveTake(int t, int s);  // enqueue an OGG save of the slot's committed take (M4)
	void clearFile(int t, int s); // enqueue a retire of the slot's live file into history/ (M4)
	// BPI halved/doubled at the same BPM: derive grid-fitting takes from mismatched
	// ones — tile a take into the doubled interval, or split it into two chained
	// halves (worker builds the buffers; guarded install through the load path).
	void maybeConvert(Track& tr, int t, int s);
	void release(Buf* b);
	// Start slot `s` playing on track `t`, emitting a START event (reason = why) and a
	// STOP(R_REPLACED) for the slot it displaces. A self-retrigger (follow → itself, or
	// launching the already-playing slot) emits nothing — the span continues.
	void setPlaying(int t, int s, uint8_t reason);
	static void refuse(Slot& sl, double now) { sl.flashAt.store(now, std::memory_order_relaxed); }
	// A playing cell steps down to FILLED — unless it has no take (an empty "rest" cell
	// playing silence in a follow chain), which goes back to EMPTY. Emits a STOP event
	// (reason = why) when the cell was actually PLAYING.
	void demote(int t, int s, uint8_t reason);
	// Push one performance event to the sink via the worker queue (audio thread; never
	// blocks — a full queue drops the event and counts it).
	void emitEvent(bool start, int t, int s, uint8_t reason);
	// One cell's CLEAR (audio thread): stop + release + retire + reset settings.
	void clearCell(int t, int s);
	// The auto-advance chain involving slot `s` broke (its cell was cleared, cancelled,
	// or replaced): if `s` was the armed cell, the predecessor becomes the performance's
	// last cell (one pass on replay, follow stays Stop). Audio thread only. Every path
	// that can kill the armed cell or the predecessor must run this, or chainFrom goes
	// stale and a later unrelated capture is misclassified as chained.
	static void breakChain(Track& tr, int s) {
		if (tr.chainFrom < 0 || (s != tr.chainFrom && s != tr.chainFrom + 1)) return;
		if (s == tr.chainFrom + 1)
			tr.slots[tr.chainFrom].repeats.store(1, std::memory_order_relaxed);
		tr.chainFrom = -1;
		tr.chainHead = -1;
	}
	void dropOverdub(Slot& sl);

	SpscQueue<Cmd, 256> cmds;      // audio → worker
	SpscQueue<Reply, 256> replies; // worker → audio
	SpscQueue<Intent, 64> intents; // UI → audio
	SpscQueue<LoadInstall, 128> loads; // worker → audio (clip loader)
	LooperWorker* worker = nullptr;

	int N = 0;
	float sr = 0.f;
	uint32_t gen = 0;
	int curBpm = 0, curBpi = 0; // the live grid's tempo (set at regrid; BPI converter)
	bool haveGrid = false;
	float gateDecay = 0.f;
	int declickN = 0;   // loop-end fade length in frames (~1.5 ms), computed on regrid
	uint32_t seqCounter = 1;
	int lastFrameRecorded = -1; // frames [0, lastFrameRecorded] of the interval are in `rec`
	int odTrack = -1, odSlot = -1; // the slot currently accumulating a continuous overdub
	// The current frame's session-timeline position, cached at the top of tick() so
	// mid-interval paths (a CLEAR intent) stamp events without the clock in scope.
	uint64_t curSession = 0;
	std::atomic<long> eventsDropped{0}; // diagnostics: events lost to a full queue
};

} // namespace looper
} // namespace akaudio
