// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
// LooperEngine — the audio-thread half of the Looper (docs/LOOPER_DESIGN.md §5):
// tracks × slots of takes, beat-quantized clip actions, free-running loop playback.
// Rack-free and driven one frame at a time by a ClockFrame (Ninjam's JamClockMessage
// in the plugin, a synthetic clock in test/).
//
// THE REALTIME CONTRACT (CLAUDE.md) applies to everything called from tick():
// no allocation, no locks, no I/O. Buffers are allocated/freed ONLY by LooperWorker;
// the audio thread asks for them (Cmd) and receives them (Reply) over SPSC queues and
// commits by pointer swap. A committed take is immutable; capture and overdub build
// into a staging buffer that is swapped in at commit.
//
// The ACTION grid is the BEAT (fluid-jamming rework, 2026-08-29): launch, stop,
// recording start and recording finish all commit on the next beat boundary — the
// interval (NINJAM's bar) matters only as the take-length cap and the auto-advance
// chain quantum. Capture has Ableton clip semantics: pressing an empty slot arms it;
// at the next BEAT it starts RECORDING (input goes straight into its staging buffer;
// the sub-beat press→beat pre-roll folds into the take's tail — the pickup); pressing
// the RECORDING cell queues a FINISH — at the next beat the take (any whole-beat
// length) commits and replays from its own start; recording that reaches one full
// interval auto-commits (hot tail → auto-advance chains into the next empty cell).
// Pressing an ARMED slot cancels the arm; the track STOP button is the discard.
//
// PLAYBACK IS FREE-RUNNING: a take keeps its recorded length and pitch forever; a
// launch starts it on a beat and it then cycles at its OWN period (repeats, decay and
// follow actions count at its wrap). A tempo change never converts audio — old takes
// keep playing at the old speed and simply drift against the new grid.
//
// Buffer accounting per track: `spare` (the chain's instant hand-off); per filled
// slot one take; per in-flight capture or overdub one staging buffer.
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
	bool beat;            // first frame of a beat, incl. the downbeat (the ACTION grid:
	                      // launch/stop/record commit on beats — sim clock: beat = downbeat)
	int beatIndex;        // 0..bpi-1 (0 when the clock has no beats)
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

// A decoded take on its way from the worker into a slot (clip loader). The worker
// allocates + fills `buf` (like a committed take) and computes the thumbnail; the
// audio thread installs it as a FILLED take. Buffer lifetime follows the usual rule —
// freed by the worker when the slot is later cleared/overwritten.
struct LoadInstall {
	int track, slot;
	Buf* buf;
	TakeMeta meta;
	float thumb[THUMB_BINS];
};

// Audio → worker.
struct Cmd {
	enum Kind { ALLOC, OVERDUB_COPY, RELEASE, SAVE, CLEAR_FILE, EVENT } kind;
	int track, slot, frames, upto;
	uint32_t seq;
	Buf* a;      // OVERDUB_COPY: the take to copy; RELEASE: the buffer; SAVE: the source take
	Buf* b;      // OVERDUB_COPY: the rolling buffer whose [0, upto) is added
	TakeMeta meta; // SAVE: the take's metadata
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
	std::atomic<bool> overdubbing{false}; // this slot is the live overdub target (UI marker)
	std::atomic<double> flashAt{-1.0}; // wall time of a refused action (UI: red flash)
	// UI progress: PLAYING = loop phase (playPos / take.frames); RECORDING = capture
	// progress (recPos / N). Written by the audio thread each frame.
	std::atomic<float> phaseA{0.f};
	float thumb[THUMB_BINS] = {};      // display only; accumulated while recording
	// Audio-thread only.
	Take take;
	// Free-running loop playhead (frames into the take). A take keeps its recorded
	// length forever: launched on a beat boundary, it then cycles at its OWN period —
	// after a tempo change it simply keeps playing at the old speed, drifting against
	// the new grid ("tough luck" by design; no re-pitching, no tile/split derivation).
	int playPos = 0;
	bool startedThisBoundary = false;
	// Capture / overdub staging (one at a time — a slot is never both). Capture:
	// requested at the press (ALLOC, capSeq), input recorded straight into it from the
	// start beat; committed at a beat (FINISH) or at the one-interval cap. Overdub:
	// a worker copy of the take (odSeq), input accumulated at the playhead, swapped in
	// at the take's own wrap.
	Buf* staging = nullptr;
	uint32_t capSeq = 0;    // outstanding capture ALLOC (0 = none)
	int recPos = -1;        // frames recorded into staging (−1 = recording not started)
	int preW = 0;           // pre-roll (pickup) frames written at the staging tail
	uint64_t recStart = 0;  // session frame of the recording's first frame
	float recPeak = 0.f;    // running peak of the recording (commit gate)
	// Stop/replace fade-out frames left (~1.5 ms): a beat-quantized cut lands anywhere
	// in a free-running loop, so the demoted slot keeps rendering, faded, until this
	// reaches 0. PER SLOT — two demotes on one track within the fade window (scene
	// launch + capture steal on one beat, a wrap-follow near a beat) each fade cleanly
	// instead of the second hard-cutting the first.
	int dieFade = 0;
	uint32_t odSeq = 0;
	float odPeak = 0.f;     // running peak of overdubbed input (merged at commit)
	bool odReady = false;
};

struct Track {
	Slot slots[MAX_SLOTS];
	std::atomic<int> playingSlot{-1};
	std::atomic<bool> present{false};
	std::atomic<int> bufs{0};        // spare buffers held — diagnostics
	// Audio-thread only.
	// One pre-allocated interval buffer: the auto-advance chain's instant staging
	// hand-off at the commit beat (a fresh ALLOC round-trip would drop frames).
	Buf* spare = nullptr;
	bool sparePending = false;
	int chainFrom = -1;   // predecessor of the auto-advance chain's armed cell (−1 = no chain)
	int chainHead = -1;   // the chain's first cell — a FINISH press launches the replay here
	bool anyDying = false; // a slot on this track has dieFade > 0 (gates the render scan)
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
	std::atomic<int> intervalFrames{0};  // current N (UI)
	std::atomic<long> allocations{0};    // buffers allocated by the worker (diagnostics/test)

private:
	friend class LooperWorker;
	void regrid(const ClockFrame& c);
	// Interval downbeat: playing REST cells (silence in a follow chain) count their
	// repeats in intervals here; real takes count at their own wrap in tick().
	void boundary(const ClockFrame& c, double now);
	// Beat boundary (incl. the downbeat): commit pending LAUNCH / STOP / CAPTURE-start /
	// FINISH — the action grid is the beat, not the interval.
	void beatCommit(const ClockFrame& c, double now);
	// Commit a recording: staging[0, recPos) becomes the take (chain / finish / silence
	// rules), from beatCommit (FINISH) or the capture step (one-interval cap).
	void commitCapture(int t, int s, bool finishing, const ClockFrame& c, double now);
	// Frames from c's frame to the start of the next beat (N - frame when beat-less).
	static int framesToNextBeat(const ClockFrame& c);
	void drainReplies();
	void drainIntents();
	void requestSpare(int t);
	void drainLoads();             // install decoded takes from the worker (clip loader)
	bool armOverdub(int t, int s); // request staging = copy(take) for the overdub cycle
	// Swap the completed overdub staging (take + input folded so far) in as the new
	// take. Safe mid-cycle: beyond the playhead the staging is bit-identical to the
	// take (the worker copied it; input lands only at already-played positions), so a
	// partial layer commits without touching unplayed audio. Does NOT queue the save —
	// callers order saveTake around other queue traffic. False = nothing to commit.
	bool commitOverdubLayer(int t, int s);
	void saveTake(int t, int s);  // enqueue an OGG save of the slot's committed take (M4)
	void clearFile(int t, int s); // enqueue a retire of the slot's live file into history/ (M4)
	// The playing slot completed one full loop cycle: advance the repeat counter, apply
	// decay, and when exhausted run the follow action. Called from tick() at the take's
	// OWN wrap (playPos reaching take.frames — the grid boundary when lengths match),
	// and from boundary() for playing REST cells (no take: silence counts in intervals).
	void wrapPlaying(int t, double now);
	void release(Buf* b);
	// Start slot `s` playing on track `t`, emitting a START event (reason = why) and a
	// STOP(R_REPLACED) for the slot it displaces. A self-retrigger (follow → itself, or
	// launching the already-playing slot) emits nothing — the span continues.
	void setPlaying(int t, int s, uint8_t reason);
	static void refuse(Slot& sl, double now) { sl.flashAt.store(now, std::memory_order_relaxed); }
	// A playing cell steps down to FILLED — unless it has no take (an empty "rest" cell
	// playing silence in a follow chain), which goes back to EMPTY. Emits a STOP event
	// (reason = why) when the cell was actually PLAYING, and starts the ~1.5 ms
	// fade-out (dyingSlot) so a mid-cycle cut can't click.
	void demote(int t, int s, uint8_t reason);
	// Release a slot's capture staging + reset the capture fields (cancel/clear/regrid).
	void dropCapture(Slot& sl);
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
	int curBpm = 0, curBpi = 0; // the live grid's tempo (set at regrid)
	bool haveGrid = false;
	float gateDecay = 0.f;
	int declickN = 0;   // loop-edge fade length in frames (~1.5 ms), computed on regrid
	uint32_t seqCounter = 1;
	int odTrack = -1, odSlot = -1; // the slot currently accumulating a continuous overdub
	// The current frame's session-timeline position, cached at the top of tick() so
	// mid-interval paths (a CLEAR intent) stamp events without the clock in scope.
	uint64_t curSession = 0;
	std::atomic<long> eventsDropped{0}; // diagnostics: events lost to a full queue
};

} // namespace looper
} // namespace akaudio
