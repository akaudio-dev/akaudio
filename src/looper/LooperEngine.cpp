// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "LooperEngine.hpp"
#include "LooperWorker.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace akaudio {
namespace looper {

// Peak-accumulate one sample into the display thumbnail (pos of len frames → bin).
static inline void thumbAccum(float* thumb, long long pos, long long len, float a) {
	int bin = (int) std::min<long long>(THUMB_BINS - 1, pos * THUMB_BINS / len);
	thumb[bin] = std::max(thumb[bin], std::min(1.f, a));
}

LooperEngine::LooperEngine() {
	worker = new LooperWorker(*this);
}

LooperEngine::~LooperEngine() {
	stop();
	delete worker; // joins + frees its pool
	// Nothing runs any more: free what the audio thread held.
	auto freeBuf = [](Buf* b) { if (b) { delete[] b->pcm; delete b; } };
	for (int t = 0; t < MAX_TRACKS; t++) {
		Track& tr = tracks[t];
		freeBuf(tr.spare);
		for (int s = 0; s < MAX_SLOTS; s++) {
			freeBuf(tr.slots[s].take.buf);
			freeBuf(tr.slots[s].staging);
		}
	}
	// Replies never collected.
	Reply r;
	while (replies.pop(r))
		freeBuf(r.buf);
	// Loads never installed.
	LoadInstall li;
	while (loads.pop(li))
		freeBuf(li.buf);
	Cmd c;
	while (cmds.pop(c))
		if (c.kind == Cmd::RELEASE)
			freeBuf(c.a);
}

void LooperEngine::start() { worker->start(); }
void LooperEngine::stop() { worker->stop(); }

// ---------------------------------------------------------------------------------
// Audio-thread helpers
// ---------------------------------------------------------------------------------

void LooperEngine::release(Buf* b) {
	if (!b)
		return;
	Cmd c {};
	c.kind = Cmd::RELEASE; c.track = c.slot = c.frames = 0; c.seq = 0; c.a = b;
	cmds.push(c); // a full queue would leak the buffer rather than block — sized so it can't happen
}

void LooperEngine::requestSpare(int t) {
	Track& tr = tracks[t];
	// The spare follows the cable: an unpatched track holds none (at 32 BPI a buffer is
	// 6-12 MB), a patched one gets its chain hand-off buffer within a few ms of plugging
	// in. slot = -1 marks the reply as a spare (a capture staging reply carries its slot).
	if (tr.sparePending || N <= 0 || !tr.present.load(std::memory_order_relaxed))
		return;
	Cmd c {};
	c.kind = Cmd::ALLOC; c.track = t; c.slot = -1; c.frames = N; c.seq = seqCounter++; c.a = nullptr;
	if (cmds.push(c))
		tr.sparePending = true;
}

// Ask the worker for a capture staging buffer for (t, s) — one in flight at a time.
// The reply is accepted in drainReplies only while the slot is still CAPTURE-armed.
void LooperEngine::requestCaptureStaging(int t, int s) {
	Slot& sl = tracks[t].slots[s];
	if (sl.capAllocPending || N <= 0)
		return;
	Cmd c {};
	c.kind = Cmd::ALLOC; c.track = t; c.slot = s; c.frames = N; c.seq = seqCounter++; c.a = nullptr;
	if (cmds.push(c))
		sl.capAllocPending = true;
}

// Hand a freshly committed take to the sink for encoding + indexing (M4). Runs on the
// audio thread but only enqueues a POD — the encode happens on the worker. The take's
// buffer stays alive for the save because its RELEASE (if it is ever overwritten) is
// pushed to the SAME queue afterwards and the worker serves the queue in order.
void LooperEngine::saveTake(int t, int s) {
	if (!sink) return;
	Slot& sl = tracks[t].slots[s];
	if (!sl.take.buf || sl.take.frames <= 0) return;
	Cmd c {};
	c.kind = Cmd::SAVE; c.track = t; c.slot = s; c.frames = sl.take.frames;
	c.seq = seqCounter++; c.a = sl.take.buf;
	c.meta.frames = sl.take.frames;
	c.meta.sampleRate = sl.take.sampleRate;
	c.meta.bpm = sl.take.bpm; c.meta.bpi = sl.take.bpi;
	c.meta.startFrame = sl.take.startFrame;
	c.meta.peak = sl.take.peak;
	c.meta.repeats = sl.repeats.load(std::memory_order_relaxed);
	c.meta.decayDb = sl.decayDb.load(std::memory_order_relaxed);
	c.meta.followSlot = sl.followSlot.load(std::memory_order_relaxed);
	cmds.push(c); // a dropped save just misses the disk copy; never blocks the audio thread
}

// Enqueue "retire this slot's live file into history/" (M4). The buffer is released
// separately; this only touches the on-disk file + manifest, on the worker.
void LooperEngine::clearFile(int t, int s) {
	if (!sink) return;
	Cmd c {};
	c.kind = Cmd::CLEAR_FILE; c.track = t; c.slot = s; c.frames = 0;
	c.seq = 0; c.a = nullptr;
	cmds.push(c);
}

// Arm a continuous overdub on (t,s): ask the worker for staging = a copy of the current
// take. The audio thread accumulates input into staging at the playhead as the loop
// plays, and the take's own wrap commits the layer (swap). Returns true if the request
// was issued.
bool LooperEngine::armOverdub(int t, int s) {
	Slot& sl = tracks[t].slots[s];
	if (!sl.take.buf || sl.take.frames <= 0)
		return false;
	Cmd cmd {};
	cmd.kind = Cmd::OVERDUB_COPY; cmd.track = t; cmd.slot = s; cmd.frames = sl.take.frames;
	cmd.seq = seqCounter++; cmd.a = sl.take.buf;
	if (!cmds.push(cmd))
		return false;
	sl.odSeq = cmd.seq;
	sl.odReady = false;
	sl.odPeak = 0.f;
	return true;
}

bool LooperEngine::commitOverdubLayer(int t, int s) {
	Slot& sl = tracks[t].slots[s];
	if (!sl.odReady || !sl.staging || !sl.take.buf
	        || sl.staging->frames != sl.take.frames)
		return false;
	release(sl.take.buf);
	sl.take.buf = sl.staging;
	sl.take.peak = std::max(sl.take.peak, sl.odPeak);
	sl.staging = nullptr;
	sl.odReady = false;
	sl.odSeq = 0;
	sl.odPeak = 0.f;
	return true;
}

void LooperEngine::dropOverdub(Slot& sl) {
	if (sl.staging) {
		release(sl.staging);
		sl.staging = nullptr;
	}
	sl.odReady = false;
	sl.odSeq = 0;
	sl.odPeak = 0.f;
}

// A recording (or its armed pre-roll) dies without committing: release the staging,
// reset the cell to EMPTY, and clear its UI remnants (fill bar, thumbnail).
void LooperEngine::discardRecording(int t, int s) {
	Slot& sl = tracks[t].slots[s];
	dropCapture(sl);
	sl.state.store(EMPTY, std::memory_order_relaxed);
	sl.phaseA.store(0.f, std::memory_order_relaxed);
	for (int b = 0; b < THUMB_BINS; b++) sl.thumb[b] = 0.f;
}

void LooperEngine::dropCapture(Slot& sl) {
	if (sl.staging) {
		release(sl.staging);
		sl.staging = nullptr;
	}
	sl.capAllocPending = false;
	sl.recPos = -1;
	sl.preW = 0;
	sl.recPeak = 0.f;
}

void LooperEngine::setPlaying(int t, int s, uint8_t reason) {
	Track& tr = tracks[t];
	int prev = tr.playingSlot.load(std::memory_order_relaxed);
	if (prev >= 0 && prev != s)
		demote(t, prev, LoopEvent::R_REPLACED);
	Slot& sl = tr.slots[s];
	sl.dieFade = 0; // a relaunch of a still-fading slot would render it twice — cut the fade
	// A self-retrigger (follow → itself, or launching the playing slot) restarts the
	// repeat counter but the audible span continues — no STOP/START pair.
	const bool retrigger = prev == s && sl.state.load(std::memory_order_relaxed) == PLAYING;
	sl.state.store(PLAYING, std::memory_order_relaxed);
	sl.repCount.store(0, std::memory_order_relaxed);
	sl.gain.store(1.f, std::memory_order_relaxed);
	sl.playPos = 0; // the loop (re)starts from its own frame 0
	sl.phaseA.store(0.f, std::memory_order_relaxed);
	sl.restStarted = sl.take.buf == nullptr; // only rest cells consume this (interval counting)
	tr.playingSlot.store(s, std::memory_order_relaxed);
	if (!retrigger)
		emitEvent(true, t, s, reason);
}

void LooperEngine::demote(int t, int s, uint8_t reason) {
	Track& tr = tracks[t];
	Slot& sl = tr.slots[s];
	if (sl.state.load(std::memory_order_relaxed) == PLAYING) {
		emitEvent(false, t, s, reason);
		// Beat-quantized stops land anywhere in a free-running loop: fade the cut loop
		// out over ~1.5 ms (the slot keeps rendering while dieFade drains) instead of
		// clicking. Per slot, so overlapping demotes on one track each fade cleanly.
		if (sl.take.buf && declickN > 0 && sl.playPos < sl.take.frames
		        && declickEnabled.load(std::memory_order_relaxed)) {
			sl.dieFade = declickN;
			tr.anyDying = true;
		}
	}
	sl.state.store(sl.take.buf ? FILLED : EMPTY, std::memory_order_relaxed);
}

void LooperEngine::emitEvent(bool start, int t, int s, uint8_t reason) {
	if (!sink) return;
	Slot& sl = tracks[t].slots[s];
	Cmd c {};
	c.kind = Cmd::EVENT; c.track = t; c.slot = s; c.frames = 0;
	c.seq = seqCounter++; c.a = nullptr;
	c.ev.start = start;
	c.ev.track = t;
	c.ev.slot = s;
	c.ev.sessionFrame = curSession;
	c.ev.takeStartFrame = sl.take.buf ? sl.take.startFrame : 0;
	c.ev.rest = sl.take.buf == nullptr;
	c.ev.bpm = curBpm;
	c.ev.bpi = curBpi;
	c.ev.sampleRate = sr;
	c.ev.gridGeneration = gen;
	c.ev.reason = reason;
	if (!cmds.push(c))
		eventsDropped.fetch_add(1, std::memory_order_relaxed);
}

void LooperEngine::drainReplies() {
	Reply r;
	while (replies.pop(r)) {
		if (r.track < 0 || r.track >= MAX_TRACKS) { release(r.buf); continue; }
		Track& tr = tracks[r.track];
		switch (r.kind) {
			case Reply::ALLOC:
				if (r.slot < 0) {
					// The track's spare (chain hand-off buffer).
					tr.sparePending = false;
					if (r.buf && r.buf->frames == N && !tr.spare)
						tr.spare = r.buf;
					else
						release(r.buf); // grid moved meanwhile, or already stocked
				} else if (r.slot < MAX_SLOTS) {
					// A capture staging: accept only while its arm is still pending.
					// (Any in-flight ALLOC for this slot is equivalent — a zeroed
					// N-frame buffer — so a plain in-flight flag replaces seq matching.)
					Slot& sl = tr.slots[r.slot];
					sl.capAllocPending = false;
					if (!sl.staging && r.buf && r.buf->frames == N
					        && sl.pending.load(std::memory_order_relaxed) == CAPTURE) {
						sl.staging = r.buf; // zeroed by the worker
					} else {
						release(r.buf);
					}
				} else {
					release(r.buf);
				}
				break;
			case Reply::OVERDUB_COPY: {
				if (r.slot < 0 || r.slot >= MAX_SLOTS) { release(r.buf); break; }
				Slot& sl = tr.slots[r.slot];
				// Accept the staging only if it answers the current arm (odSeq) and still
				// matches the take; a superseded / cancelled / replaced request is recycled.
				if (sl.odSeq == r.seq && sl.odSeq != 0 && r.buf && !sl.staging
				        && sl.take.buf && r.buf->frames == sl.take.frames) {
					sl.staging = r.buf;
					sl.odReady = true;
				} else {
					release(r.buf);
				}
				break;
			}
		}
		tr.bufs.store(tr.spare ? 1 : 0, std::memory_order_relaxed);
	}
}

// One cell's CLEAR (audio thread): stop + release + retire + reset settings.
void LooperEngine::clearCell(int t, int s) {
	Track& tr = tracks[t];
	Slot& sl = tr.slots[s];
	sl.pending.store(NONE, std::memory_order_relaxed);
	dropOverdub(sl);
	dropCapture(sl);
	sl.overdubbing.store(false, std::memory_order_relaxed);
	if (odTrack == t && odSlot == s) { odTrack = odSlot = -1; }
	sl.dieFade = 0; // a relaunch of a still-fading slot would render it twice // its buffer dies now
	if (sl.state.load(std::memory_order_relaxed) == RECORDING)
		sl.state.store(EMPTY, std::memory_order_relaxed);
	if (tr.playingSlot.load(std::memory_order_relaxed) == s) {
		// The playing cell dies mid-interval: close its span now (stamped
		// with the current frame, not the last boundary).
		if (sl.state.load(std::memory_order_relaxed) == PLAYING)
			emitEvent(false, t, s, LoopEvent::R_CLEAR);
		tr.playingSlot.store(-1, std::memory_order_relaxed);
	}
	const bool hadTake = sl.take.buf != nullptr;
	if (sl.take.buf) { release(sl.take.buf); sl.take = Take(); }
	if (hadTake) clearFile(t, s); // retire the live file into history/ (M4)
	breakChain(tr, s); // clearing a chain member ends the chain
	sl.state.store(EMPTY, std::memory_order_relaxed);
	sl.playPos = 0;
	sl.phaseA.store(0.f, std::memory_order_relaxed);
	// Settings reset too: an empty cell's settings are visible now (a
	// "rest" step in a follow chain), so Clear must not leave a ghost.
	sl.repeats.store(0, std::memory_order_relaxed);
	sl.decayDb.store(0.f, std::memory_order_relaxed);
	sl.followSlot.store(0, std::memory_order_relaxed);
	for (int b = 0; b < THUMB_BINS; b++) sl.thumb[b] = 0.f;
}

void LooperEngine::drainIntents() {
	Intent i;
	while (intents.pop(i)) {
		if (i.track < 0 || i.track >= MAX_TRACKS || i.slot < 0 || i.slot >= MAX_SLOTS) continue;
		switch (i.kind) {
			case Intent::CLEAR:
				clearCell(i.track, i.slot);
				break;
			case Intent::CLEAR_ALL:
				// The whole grid, as one intent — a 64-cell burst of per-cell intents
				// would overflow the 63-slot queue and silently drop the last.
				for (int t = 0; t < MAX_TRACKS; t++)
					for (int s = 0; s < MAX_SLOTS; s++)
						clearCell(t, s);
				break;
			case Intent::CARRY_SPANS: {
				// The session migrated to a fresh folder (adoption): the files and
				// manifest rows traveled via Session::migrateTo; here we only re-open
				// the playing cells' spans in the new folder's events log, so the
				// as-played timeline starts whole.
				for (int t = 0; t < MAX_TRACKS; t++)
					for (int s = 0; s < MAX_SLOTS; s++)
						if (tracks[t].slots[s].state.load(std::memory_order_relaxed) == PLAYING)
							emitEvent(true, t, s, LoopEvent::R_CARRY);
				break;
			}
		}
	}
}

// Install decoded takes handed over by the worker (clip loader, v2). Each becomes a
// FILLED slot with its saved settings, launchable regardless of the live grid — a
// restored take keeps its recorded length and free-runs like any other. Buffer
// ownership passes to the slot (freed on clear/overwrite). Audio thread.
void LooperEngine::drainLoads() {
	LoadInstall li;
	while (loads.pop(li)) {
		if (li.track < 0 || li.track >= MAX_TRACKS || li.slot < 0 || li.slot >= MAX_SLOTS) {
			release(li.buf);
			continue;
		}
		Track& ltr = tracks[li.track];
		Slot& sl = ltr.slots[li.slot];
		sl.dieFade = 0; // buffer replaced — stop rendering the fade
		// A load can land on a slot the user started using meanwhile (restore decodes
		// arrive seconds after patch load): tear the slot down the way clearCell
		// would — close a playing span, disarm capture/overdub, release stagings —
		// or the slot ends up FILLED-but-playing (unstoppable: STOP gates on
		// PLAYING) with a stranded staging buffer.
		sl.pending.store(NONE, std::memory_order_relaxed);
		dropOverdub(sl);
		dropCapture(sl);
		sl.overdubbing.store(false, std::memory_order_relaxed);
		if (odTrack == li.track && odSlot == li.slot) { odTrack = odSlot = -1; }
		if (ltr.playingSlot.load(std::memory_order_relaxed) == li.slot) {
			if (sl.state.load(std::memory_order_relaxed) == PLAYING)
				emitEvent(false, li.track, li.slot, LoopEvent::R_REPLACED);
			ltr.playingSlot.store(-1, std::memory_order_relaxed);
		}
		breakChain(ltr, li.slot); // a load landing on a chain member ends the chain
		if (sl.take.buf) release(sl.take.buf); // replace whatever is there
		sl.take.buf = li.buf;
		sl.take.frames = li.buf->frames; // the buffer's real length — the take's own period
		sl.take.bpm = li.meta.bpm;
		sl.take.bpi = li.meta.bpi;
		sl.take.sampleRate = li.meta.sampleRate;
		sl.take.startFrame = li.meta.startFrame;
		sl.take.peak = li.meta.peak;
		sl.repeats.store(li.meta.repeats, std::memory_order_relaxed);
		sl.decayDb.store(li.meta.decayDb, std::memory_order_relaxed);
		sl.followSlot.store(li.meta.followSlot, std::memory_order_relaxed);
		for (int b = 0; b < THUMB_BINS; b++) sl.thumb[b] = li.thumb[b];
		sl.playPos = 0;
		sl.repCount.store(0, std::memory_order_relaxed);
		sl.gain.store(1.f, std::memory_order_relaxed);
		sl.pending.store(NONE, std::memory_order_relaxed);
		sl.state.store(FILLED, std::memory_order_relaxed);
	}
}

// The grid moved (first clock, join, tempo change, sample rate): queued actions were
// aimed at the old grid — cancel them; in-flight recordings and stagings are sized to
// the old N — discard; the spare goes back to the worker if mis-sized. PLAYING TAKES
// KEEP PLAYING at their own recorded speed — a tempo change never converts or stops
// committed audio (only a playing "rest" cell, which is pure grid silence, is demoted).
void LooperEngine::regrid(const ClockFrame& c) {
	N = c.intervalFrames;
	sr = c.sampleRate;
	gen = c.gridGeneration;
	curBpm = c.bpm;
	curBpi = c.bpi;
	haveGrid = true;
	for (int t = 0; t < MAX_TRACKS; t++) { tracks[t].chainFrom = -1; tracks[t].chainHead = -1; } // chains die with the old grid
	intervalFrames.store(N, std::memory_order_relaxed);
	gateDecay = sr > 0.f ? std::exp(-1.f / (0.1f * sr)) : 0.f; // ~100 ms release
	declickN = N > 0 ? std::max(1, std::min(N / 4, (int) (sr * 0.0015f))) : 0; // ~1.5 ms loop-edge fade
	for (int t = 0; t < MAX_TRACKS; t++) {
		Track& tr = tracks[t];
		if (tr.spare && tr.spare->frames != N) { release(tr.spare); tr.spare = nullptr; }
		tr.anyDying = false;
		for (int s = 0; s < MAX_SLOTS; s++) {
			Slot& sl = tr.slots[s];
			sl.pending.store(NONE, std::memory_order_relaxed);
			dropOverdub(sl);
			dropCapture(sl);
			sl.overdubbing.store(false, std::memory_order_relaxed);
			if (sl.state.load(std::memory_order_relaxed) == RECORDING)
				sl.state.store(EMPTY, std::memory_order_relaxed);
			if (sl.state.load(std::memory_order_relaxed) == PLAYING && !sl.take.buf) {
				demote(t, s, LoopEvent::R_REGRID); // a playing rest cell goes back to EMPTY
				if (tr.playingSlot.load(std::memory_order_relaxed) == s)
					tr.playingSlot.store(-1, std::memory_order_relaxed);
			}
		}
		if (!tr.spare)
			requestSpare(t);
		tr.bufs.store(tr.spare ? 1 : 0, std::memory_order_relaxed);
	}
	odTrack = odSlot = -1; // the overdub re-targets at the next frame
}

// Frames from this clock frame to the start of the next beat. Beat b starts at
// ceil(b*N/bpi) — the same integer grid JamClock reports beatIndex against — so the
// pre-roll writer and the beat flag agree exactly. A beat-less clock (sim) has one
// action point per interval: the downbeat.
int LooperEngine::framesToNextBeat(const ClockFrame& c) {
	const int n = c.intervalFrames;
	if (n <= 0) return 0;
	if (c.bpi <= 1)
		return n - c.frameInInterval;
	const int b = c.beatIndex;
	const int next = b + 1 >= c.bpi
		? n
		: (int) (((long long) (b + 1) * n + c.bpi - 1) / c.bpi);
	return next - c.frameInInterval;
}

// Beat boundary (incl. the downbeat): the ACTION grid. Commit pending LAUNCH / STOP,
// start pending recordings, and commit pending FINISHes.
void LooperEngine::beatCommit(const ClockFrame& c, double now) {
	for (int t = 0; t < MAX_TRACKS; t++) {
		Track& tr = tracks[t];
		const bool present = tr.present.load(std::memory_order_relaxed);
		for (int s = 0; s < MAX_SLOTS; s++) {
			Slot& sl = tr.slots[s];
			const int p = sl.pending.load(std::memory_order_relaxed);
			if (p == NONE) continue;
			switch (p) {
				case CAPTURE: {
					// Recording starts on this beat — mid-interval is fine. It needs its
					// staging buffer (requested at the press; the worker round-trip is
					// ms against a beat of hundreds) — if it hasn't landed, stay armed
					// for the next beat and drop the now-misaimed pre-roll.
					if (!present) {
						sl.pending.store(NONE, std::memory_order_relaxed);
						dropCapture(sl); // the delivered staging must not stay parked on an EMPTY slot
						refuse(sl, now);
						break;
					}
					if (!sl.staging) {
						sl.preW = 0; // the pre-roll aimed at THIS beat; re-aim at the next
						// Pressed before the grid was up (or the queue was full):
						// (re)issue the staging request and stay armed.
						requestCaptureStaging(t, s);
						break;
					}
					sl.pending.store(NONE, std::memory_order_relaxed);
					// Recording takes over the track: the old loop is never audible
					// under the instrument being recorded.
					int ps = tr.playingSlot.load(std::memory_order_relaxed);
					if (ps >= 0) {
						demote(t, ps, LoopEvent::R_STEAL);
						tr.playingSlot.store(-1, std::memory_order_relaxed);
					}
					sl.state.store(RECORDING, std::memory_order_relaxed);
					sl.recPos = 0;
					sl.recPeak = 0.f;
					sl.recStart = c.sessionFrame;
					break;
				}
				case FINISH:
					sl.pending.store(NONE, std::memory_order_relaxed);
					// The take ends on this beat, whatever its length in beats: commit
					// [0, recPos) and replay — a chain closes and cycles from its head.
					if (sl.state.load(std::memory_order_relaxed) == RECORDING)
						commitCapture(t, s, true, c, now);
					break;
				case LAUNCH:
					sl.pending.store(NONE, std::memory_order_relaxed);
					// Any take launches — length vs the live grid is irrelevant: the
					// loop free-runs at its own period from this beat.
					if (!sl.take.buf) { refuse(sl, now); break; }
					setPlaying(t, s, LoopEvent::R_LAUNCH);
					break;
				case STOP:
					sl.pending.store(NONE, std::memory_order_relaxed);
					if (sl.state.load(std::memory_order_relaxed) == PLAYING) {
						demote(t, s, LoopEvent::R_STOP);
						if (tr.playingSlot.load(std::memory_order_relaxed) == s)
							tr.playingSlot.store(-1, std::memory_order_relaxed);
					}
					break;
				default:
					break;
			}
		}
	}
}

// Commit a recording: staging[0, recPos) becomes the slot's take. Shared by the FINISH
// path (beatCommit — any whole-beat length) and the capture step (recPos reached the
// one-interval cap). Carries the auto-advance chain and the silence gates.
void LooperEngine::commitCapture(int t, int s, bool finishing, const ClockFrame& c, double now) {
	Track& tr = tracks[t];
	Slot& sl = tr.slots[s];
	const int len = sl.recPos;
	const bool present = tr.present.load(std::memory_order_relaxed);
	// Silence gate (−70 dBFS): a take of pure room noise is refused. A chained finish
	// demands real content in the final cell via the hotter −40 dB tail gate: the common
	// gesture is "stop playing, then click", and by then the in-flight cell holds only
	// decay — committing it would append a dead bar to the performance.
	const bool commitOk = present && sl.staging && len > 0 && sl.recPeak >= GATE
	        && (!finishing || tr.chainFrom < 0 || sl.recPeak >= TAIL_GATE);
	if (!commitOk) {
		discardRecording(t, s);
		if (finishing && tr.chainFrom >= 0) {
			// Finish with an empty final cell: no flash (the drop is the expected
			// outcome), close the chain at the predecessor and cycle the performance
			// from its head, starting this beat.
			const int head = tr.chainHead >= 0 ? tr.chainHead : tr.chainFrom;
			const int pred = tr.chainFrom;
			breakChain(tr, s); // predecessor stamped ×1, chain closed
			Slot& pd = tr.slots[pred];
			if (pred == head) {
				// The whole performance is one cell: a plain looping take, no
				// follow indirection.
				pd.repeats.store(0, std::memory_order_relaxed);
				pd.followSlot.store(0, std::memory_order_relaxed);
			} else {
				pd.followSlot.store(head + 1, std::memory_order_relaxed); // 1-based
			}
			replayChainHead(t, head, now);
		} else {
			// Silence (or the cable is gone): refuse with a flash; breakChain stamps
			// the predecessor as the performance's last cell.
			refuse(sl, now);
			breakChain(tr, s);
		}
		return;
	}
	// Staging becomes the take — no copy. The tail pre-roll (pickup) was folded in
	// place by the capture writer; on a finish-shortened take the pre-roll region lies
	// beyond `len` and is simply dropped with the unused part of the buffer.
	if (sl.take.buf) release(sl.take.buf);
	sl.take.buf = sl.staging;
	sl.staging = nullptr;
	// The take's length is the truth EVERYWHERE, including Buf::frames: the worker's
	// OVERDUB_COPY guard compares `a->frames == frames` — a finish-shortened take whose
	// buffer still said N would fail it and get a ZEROED copy, erasing the take at the
	// next overdub commit (found in review, 2026-08-29). The over-allocation past `len`
	// is simply unused; recycle() won't pool a non-N length, it deletes it.
	sl.take.buf->frames = len;
	sl.take.frames = len;
	sl.take.bpm = c.bpm; sl.take.bpi = c.bpi; sl.take.sampleRate = sr;
	sl.take.startFrame = sl.recStart;
	sl.take.peak = sl.recPeak;
	sl.capAllocPending = false;
	sl.recPos = -1;
	sl.recPeak = 0.f;
	// Thumbnail bins were laid against the one-interval cap; a shorter take re-spreads
	// its recorded prefix across the full strip.
	if (len < N && len > 0) {
		float tmp[THUMB_BINS];
		std::memcpy(tmp, sl.thumb, sizeof(tmp));
		for (int d = 0; d < THUMB_BINS; d++) {
			int lo = (int) ((long long) d * len / N);
			int hi = (int) (((long long) (d + 1) * len + (long long) N - 1) / N);
			if (hi <= lo) hi = lo + 1;
			float m = 0.f;
			for (int b = lo; b < hi && b < THUMB_BINS; b++) m = std::max(m, tmp[b]);
			sl.thumb[d] = m;
		}
	}
	sl.preW = 0;
	sl.repeats.store(defRepeats.load(std::memory_order_relaxed), std::memory_order_relaxed);
	sl.decayDb.store(defDecayDb.load(std::memory_order_relaxed), std::memory_order_relaxed);
	sl.followSlot.store(0, std::memory_order_relaxed); // a fresh take never inherits a stale follow
	// This cell was recorded as the continuation of an auto-advance chain: wire the
	// predecessor (repeats 1, follow → here) so the whole performance replays in order
	// from its first cell. Wiring happens only when the successor really commits, so a
	// follow never dangles on a refused cell.
	const bool chainedIn = tr.chainFrom >= 0 && s == tr.chainFrom + 1;
	if (chainedIn) {
		Slot& p = tr.slots[tr.chainFrom];
		p.repeats.store(1, std::memory_order_relaxed);
		p.followSlot.store(s + 1, std::memory_order_relaxed); // 1-based
	}
	// Auto-advance capture: only a take that reached the full-interval cap can chain —
	// if the player blew through the cap (the take's tail is hot), the performance
	// isn't over: keep recording into the next empty slot below, seamlessly, using the
	// track's spare as the new staging. The committed cell stays FILLED and silent
	// (recording still owns the track). The chain ends when a cell is silent (the gate
	// above refuses it), the next slot is occupied, or the column runs out. A quiet
	// tail = the player stopped before the loop point: the take starts looping right
	// now (Ableton clip semantics, the common case). The OVERDUB latch suppresses the
	// chain (playing through then layers instead), as does a FINISH press ("this cell
	// is the last", hot tail or not).
	bool chain = false;
	if (!finishing && len == N
	        && autoAdvance.load(std::memory_order_relaxed)
	        && !overdubMode.load(std::memory_order_relaxed)
	        && s + 1 < MAX_SLOTS && tr.spare
	        && tr.slots[s + 1].state.load(std::memory_order_relaxed) == EMPTY) {
		int tw = std::min((int) (TAIL_SECONDS * sr), N / 2);
		if (tw < 1) tw = 1;
		float tp = 0.f;
		const float* pcm = sl.take.buf->pcm + (size_t) (len - tw) * 2;
		for (int f = 0; f < tw * 2; f++) tp = std::max(tp, std::fabs(pcm[f]));
		chain = tp >= TAIL_GATE;
	}
	if (finishing && chainedIn) {
		// The finish closes the chain: this cell is the performance's final bar — wire
		// it back to the head so the whole take cycles, and start the replay from the
		// top on this same beat.
		const int head = tr.chainHead >= 0 ? tr.chainHead : tr.chainFrom;
		sl.repeats.store(1, std::memory_order_relaxed);
		sl.followSlot.store(head + 1, std::memory_order_relaxed); // 1-based
		sl.state.store(FILLED, std::memory_order_relaxed);
		replayChainHead(t, head, now);
	} else if (chain) {
		sl.state.store(FILLED, std::memory_order_relaxed);
		// Hand the spare to the next cell and keep recording without missing a frame.
		Slot& nx = tr.slots[s + 1];
		nx.pending.store(NONE, std::memory_order_relaxed);
		dropCapture(nx); // an orphaned staging from an aborted arm must not leak under the hand-off
		nx.staging = tr.spare; // ALLOC-zeroed, untouched since
		tr.spare = nullptr;
		nx.state.store(RECORDING, std::memory_order_relaxed);
		nx.recPos = 0;
		nx.recPeak = 0.f;
		nx.recStart = c.sessionFrame;
		nx.preW = 0;
		for (int b = 0; b < THUMB_BINS; b++) nx.thumb[b] = 0.f;
		if (!chainedIn)
			tr.chainHead = s; // a chain just started: this cell is the performance's first bar
		tr.chainFrom = s;
		requestSpare(t); // restock for the next hop
	} else if (chainedIn) {
		// A quiet tail ends the chain here: this cell is the performance's outro — one
		// pass on replay, and nothing auto-plays now (chain end leaves the track stopped).
		sl.repeats.store(1, std::memory_order_relaxed);
		sl.state.store(FILLED, std::memory_order_relaxed);
		tr.chainFrom = -1;
		tr.chainHead = -1;
	} else {
		// Plain capture — and an unchained FINISH lands here too: commit + play from
		// the take's own start, exactly the Ableton "click the recording clip" gesture.
		setPlaying(t, s, LoopEvent::R_CAPTURE);
	}
	saveTake(t, s); // encode + index the committed take (M4)
}

// Close the auto-advance chain and start the replay from its head on this same beat.
// The caller has already wired the closing cell; head < 0 or a cleared head refuses.
void LooperEngine::replayChainHead(int t, int head, double now) {
	Track& tr = tracks[t];
	tr.chainFrom = -1;
	tr.chainHead = -1;
	if (head < 0 || head >= MAX_SLOTS)
		return;
	if (tr.slots[head].take.buf)
		setPlaying(t, head, LoopEvent::R_LAUNCH);
	else
		refuse(tr.slots[head], now); // head cleared mid-chain: nothing to replay
}

// Interval downbeat. Playing REST cells (silence steps in a follow chain — they have no
// take, so no loop period of their own) count their repeats here, in intervals. Real
// takes count at their own wrap in tick()'s playback path.
void LooperEngine::boundary(double now) {
	for (int t = 0; t < MAX_TRACKS; t++) {
		Track& tr = tracks[t];
		int ps = tr.playingSlot.load(std::memory_order_relaxed);
		if (ps < 0) continue;
		Slot& sl = tr.slots[ps];
		if (sl.restStarted)
			sl.restStarted = false; // not on the downbeat it started on
		else if (!sl.take.buf)
			wrapPlaying(t, now);
	}
}

// One full loop cycle completed on the playing slot: repeats + decay, then the follow
// action when done. 0 = stop; 1..8 = launch that slot on this track (self = retrigger
// at full gain — setPlaying resets repCount/gain). An EMPTY target is a "rest": it
// plays silence for its own repeats (counted per interval), then runs its own follow —
// chains may pass through gaps. Explicit user action wins: a LAUNCH/STOP committed on
// this same frame already retargeted playingSlot, and this runs only for the slot that
// is still playing.
void LooperEngine::wrapPlaying(int t, double now) {
	Track& tr = tracks[t];
	int ps = tr.playingSlot.load(std::memory_order_relaxed);
	if (ps < 0) return;
	Slot& sl = tr.slots[ps];
	int rc = sl.repCount.load(std::memory_order_relaxed) + 1;
	sl.repCount.store(rc, std::memory_order_relaxed);
	float g = std::pow(10.f, sl.decayDb.load(std::memory_order_relaxed) * (float) rc / 20.f);
	sl.gain.store(g, std::memory_order_relaxed);
	int reps = sl.repeats.load(std::memory_order_relaxed);
	if ((reps > 0 && rc >= reps) || g < 1e-3f) {
		// Done (repeats exhausted / decayed below −60 dB): follow action. A RECORDING
		// target (chain-armed) must not be hijacked into playback — the recording wins.
		int fs = sl.followSlot.load(std::memory_order_relaxed) - 1;
		if (fs >= 0 && fs < MAX_SLOTS
		        && tr.slots[fs].state.load(std::memory_order_relaxed) != RECORDING) {
			setPlaying(t, fs, LoopEvent::R_FOLLOW);
			// A rest target counts per interval from the NEXT downbeat: consume the
			// started flag so a mid-interval follow doesn't stretch its first step.
			tr.slots[fs].restStarted = false;
		} else {
			demote(t, ps, LoopEvent::R_EXHAUSTED);
			tr.playingSlot.store(-1, std::memory_order_relaxed);
			if (fs >= 0 && fs < MAX_SLOTS)
				refuse(tr.slots[fs], now); // recording target: flash it, stop
		}
	}
}

// ---------------------------------------------------------------------------------
// Per-frame
// ---------------------------------------------------------------------------------

void LooperEngine::tick(const ClockFrame& c, const TrackIn* in, int nTracks, double now,
                        float& outL, float& outR, float& cueL, float& cueR, float* trackOutLR,
                        bool wantMix) {
	curSession = c.sessionFrame; // before the drains: a CLEAR intent stamps its event with this
	drainReplies();
	drainIntents();
	drainLoads();
	if (c.running && c.intervalFrames > 0) {
		if (!haveGrid || c.gridGeneration != gen || c.intervalFrames != N || c.sampleRate != sr)
			regrid(c);
		if (c.beat)
			beatCommit(c, now); // the action grid: launches, stops, record start/finish
		if (c.downbeat)
			boundary(now); // rest cells count their repeats per interval
	}
	const bool grid = haveGrid && N > 0 && c.running;
	const int f = c.frameInInterval;
	const bool inGrid = grid && f >= 0 && f < N;

	const bool declick = declickEnabled.load(std::memory_order_relaxed) && declickN > 0;
	int remToBeat = -1; // framesToNextBeat(c), computed once on demand (tick-invariant)

	float mixL = 0.f, mixR = 0.f, cuL = 0.f, cuR = 0.f;
	for (int t = 0; t < MAX_TRACKS; t++) {
		Track& tr = tracks[t];
		const bool has = t < nTracks;
		const bool present = has && in[t].present;
		tr.present.store(present, std::memory_order_relaxed);
		if (present && grid && !tr.spare)
			requestSpare(t); // no-op while a request is in flight
		const float l = present ? in[t].l : 0.f;
		const float r = present ? in[t].r : 0.f;
		const float a = std::max(std::fabs(l), std::fabs(r));

		// ---- Capture: the RECORDING slot writes input straight into its staging; an
		// ARMED slot records the sub-beat press→beat pre-roll into the staging TAIL —
		// the pickup: a loop is circular, so audio performed just before the take's
		// start belongs right before each repeat's downbeat. The recording-proper
		// FOLDS (+=) over that tail region when it reaches the final pre-roll frames
		// of a full-cap take, so an early hit survives the loop point at zero cost.
		if (inGrid) {
			for (int s = 0; s < MAX_SLOTS; s++) {
				Slot& sl = tr.slots[s];
				if (sl.state.load(std::memory_order_relaxed) == RECORDING && sl.staging && sl.recPos >= 0) {
					if (sl.recPos >= N) {
						// One full interval: the cap. Commit BEFORE writing this frame —
						// it belongs to the next cell when the chain rolls on.
						commitCapture(t, s, false, c, now);
						continue; // a chained successor is written when the scan reaches it
					}
					float* o = sl.staging->pcm + (size_t) sl.recPos * 2;
					if (sl.recPos >= N - sl.preW) { o[0] += l; o[1] += r; } // fold over the pickup tail
					else                          { o[0] = l;  o[1] = r;  }
					float aa = std::max(std::fabs(o[0]), std::fabs(o[1]));
					if (aa > sl.recPeak) sl.recPeak = aa;
					thumbAccum(sl.thumb, sl.recPos, N, aa);
					sl.recPos++;
					// The UI reads at ~60 Hz: publishing every 256th frame (~5 ms) is
					// already sub-visible — no need for an fdiv + store per frame.
					if (!(sl.recPos & 0xFF))
						sl.phaseA.store((float) sl.recPos / (float) N, std::memory_order_relaxed);
				} else if (sl.pending.load(std::memory_order_relaxed) == CAPTURE && sl.staging) {
					// Pre-roll: frames land at their final tail position directly —
					// N - (frames remaining to the start beat) — faded in so the fold
					// can't click at its left edge.
					if (remToBeat < 0)
						remToBeat = framesToNextBeat(c);
					const int rem = remToBeat;
					if (rem >= 1 && rem <= N) {
						const int pos = N - rem;
						float fg = declick && sl.preW < declickN
						           ? (float) (sl.preW + 1) / (float) declickN : 1.f;
						float* o = sl.staging->pcm + (size_t) pos * 2;
						o[0] = l * fg;
						o[1] = r * fg;
						sl.preW++;
						thumbAccum(sl.thumb, pos, N, a);
					}
				}
			}
		}

		// ---- Loop playback: FREE-RUNNING — the playing slot renders at its own
		// playhead and wraps at its own length (== the grid boundary only while the
		// take matches the live tempo). The wrap commits an armed overdub layer and
		// advances repeats/decay/follow.
		float loopL = 0.f, loopR = 0.f;
		int ps = tr.playingSlot.load(std::memory_order_relaxed);
		if (ps >= 0 && inGrid) {
			Slot* sl = &tr.slots[ps];
			if (sl->take.buf && sl->playPos >= sl->take.frames) {
				// Wrap: one full cycle done. Commit the continuous overdub at the loop
				// point (staging = take + this cycle's input, built below), then count.
				if (odTrack == t && odSlot == ps && commitOverdubLayer(t, ps)) {
					// Arm the NEXT cycle's copy BEFORE queueing the save: both ride the
					// same FIFO worker queue and the save is a full OGG encode (~100 ms
					// for a long interval) — copy-first shrinks each cycle's input hole
					// from encode-length to copy-length (~1 ms, hidden under the
					// loop-edge declick).
					armOverdub(t, ps);
					saveTake(t, ps); // the overdubbed take replaces the file (old → history)
				}
				sl->playPos = 0;
				wrapPlaying(t, now);
				ps = tr.playingSlot.load(std::memory_order_relaxed);
				sl = ps >= 0 ? &tr.slots[ps] : nullptr;
			}
			if (sl && sl->take.buf && sl->playPos < sl->take.frames) {
				// Continuous overdub: fold this frame's input into staging at the
				// playhead (a few ms after the copy arrives; the hole hides under the
				// loop-edge declick).
				if (odTrack == t && odSlot == ps && sl->odReady && sl->staging && present
				        && sl->staging->frames == sl->take.frames) {
					float* o = sl->staging->pcm + (size_t) sl->playPos * 2;
					o[0] += l;
					o[1] += r;
					float aa = std::max(std::fabs(o[0]), std::fabs(o[1]));
					if (aa > sl->odPeak) sl->odPeak = aa;
					thumbAccum(sl->thumb, sl->playPos, sl->take.frames, aa);
				}
				float g = sl->gain.load(std::memory_order_relaxed);
				// Declick: fade the first/last declickN frames of the cycle smoothly to 0, so
				// the loop point is continuous through zero — no click. The smoothstep has
				// zero slope at the ends, so even a very short fade is clean.
				if (declick) {
					const int nf = sl->take.frames;
					const int pp = sl->playPos;
					float ft = 1.f;
					if (pp < declickN)              ft = (pp + 0.5f) / declickN;
					else if (pp >= nf - declickN)   ft = (nf - pp - 0.5f) / declickN;
					if (ft < 1.f) g *= ft * ft * (3.f - 2.f * ft);
				}
				loopL = sl->take.buf->pcm[(size_t) sl->playPos * 2] * g;
				loopR = sl->take.buf->pcm[(size_t) sl->playPos * 2 + 1] * g;
				sl->playPos++;
				if (!(sl->playPos & 0xFF)) // ~60 Hz UI reader; 256-frame granularity suffices
					sl->phaseA.store((float) sl->playPos / (float) sl->take.frames, std::memory_order_relaxed);
			}
		}
		// Stopped/replaced loops fade out (~1.5 ms) instead of hard-cutting: every slot
		// with fade left keeps rendering on top. The scan runs only while a fade is
		// live (anyDying), i.e. for ~72 frames after a demote — not per idle frame.
		if (tr.anyDying) {
			bool any = false;
			for (int ss = 0; ss < MAX_SLOTS; ss++) {
				Slot& ds = tr.slots[ss];
				if (ds.dieFade <= 0) continue;
				if (ds.take.buf && ds.playPos < ds.take.frames && declickN > 0) {
					float ft = (float) ds.dieFade / (float) declickN;
					float g = ds.gain.load(std::memory_order_relaxed) * ft * ft * (3.f - 2.f * ft);
					loopL += ds.take.buf->pcm[(size_t) ds.playPos * 2] * g;
					loopR += ds.take.buf->pcm[(size_t) ds.playPos * 2 + 1] * g;
					ds.playPos++;
					ds.dieFade--;
					if (ds.dieFade > 0 && ds.playPos < ds.take.frames) any = true;
				} else {
					ds.dieFade = 0;
				}
			}
			tr.anyDying = any;
		}

		// Live-thru: gated to exact zero (−70 dBFS, ~100 ms release) so idle tracks cost
		// nothing on the wire, and switched by the TX latch with a ~10 ms fade.
		tr.gateEnv = std::max(a, tr.gateEnv * gateDecay);
		const bool open = tr.gateEnv >= GATE;
		float txT = (has && in[t].tx) ? 1.f : 0.f;
		tr.txGain += (txT - tr.txGain) * 0.002f;
		// Snap the exponential fade so a private track is EXACTLY silent on the wire
		// (and an on-air one exactly unity), instead of trailing 1e-9s forever.
		if (std::fabs(tr.txGain - txT) < 1e-4f) tr.txGain = txT;
		// Live-thru routes by the TX latch, crossfaded on txGain (~10 ms, no click):
		// on-air share → MIX (what the room hears), private share → CUE (your monitor).
		// Loops always go to MIX — a recorded loop is the band's committed contribution,
		// heard by the room regardless of the live TX state.
		float thruGateL = open ? l : 0.f, thruGateR = open ? r : 0.f;
		if (wantMix) {
			mixL += thruGateL * tr.txGain + loopL;
			mixR += thruGateR * tr.txGain + loopR;
		}
		cuL  += thruGateL * (1.f - tr.txGain);
		cuR  += thruGateR * (1.f - tr.txGain);
		// Per-track direct out (POLY): this channel's full output = its loop + its live-thru,
		// regardless of TX routing — a clean per-instrument stem. No limiter (individual send).
		if (trackOutLR && t < nTracks) {
			trackOutLR[(size_t) t * 2]     = loopL + thruGateL;
			trackOutLR[(size_t) t * 2 + 1] = loopR + thruGateR;
		}
	}

	// Continuous overdub retarget — immediate, mid-cycle: while the OVERDUB latch is on,
	// the selected playing (patched) cell is the target; layers commit at the take's own
	// wrap. Selection changes (or the latch dropping, or the target stopping) move/stop
	// it right away — and the PARTIAL cycle's layer commits first, so the phrase played
	// since the last wrap is kept (the layered input just ends at the commit point; on
	// later cycles that edge sits under whatever was already in the take there).
	{
		int sel = overdubMode.load(std::memory_order_relaxed)
		          ? overdubSel.load(std::memory_order_relaxed) : -1;
		int dt = -1, dsl = -1;
		if (sel >= 0 && sel < MAX_TRACKS * MAX_SLOTS) { dt = sel / MAX_SLOTS; dsl = sel % MAX_SLOTS; }
		if (odTrack != dt || odSlot != dsl) {
			if (odTrack >= 0 && odSlot >= 0) {
				Slot& o = tracks[odTrack].slots[odSlot];
				if (commitOverdubLayer(odTrack, odSlot))
					saveTake(odTrack, odSlot);
				dropOverdub(o); // no-op after a commit; recycles a not-yet-ready copy otherwise
				o.overdubbing.store(false, std::memory_order_relaxed);
			}
			odTrack = odSlot = -1;
			if (dt >= 0) {
				Track& dtr = tracks[dt];
				Slot& dslot = dtr.slots[dsl];
				if (dtr.present.load(std::memory_order_relaxed)
				        && dslot.state.load(std::memory_order_relaxed) == PLAYING
				        && dslot.take.buf && armOverdub(dt, dsl)) {
					odTrack = dt;
					odSlot = dsl;
					dslot.overdubbing.store(true, std::memory_order_relaxed);
				}
			}
		} else if (odTrack >= 0) {
			// The target stopped playing (stop, steal, exhaust): keep the partial layer,
			// end the overdub.
			Slot& o = tracks[odTrack].slots[odSlot];
			if (o.state.load(std::memory_order_relaxed) != PLAYING) {
				if (commitOverdubLayer(odTrack, odSlot))
					saveTake(odTrack, odSlot);
				dropOverdub(o);
				o.overdubbing.store(false, std::memory_order_relaxed);
				odTrack = odSlot = -1;
			}
		}
	}

	// Soft limiter: fully transparent up to full scale (internal 1.0 = ±5V), so a single
	// track passes clean; only a SUM that exceeds full scale (a loop stack, several live
	// tracks) is gently compressed, asymptoting to ~±7.5V so it never clips hard. It is
	// a safety net against stacking, NOT a level control — instruments arrive leveled.
	auto lim = [](float x) -> float {
		float ax = std::fabs(x);
		if (ax <= 1.0f) return x;
		float y = 1.0f + 0.5f * std::tanh((ax - 1.0f) / 0.5f);
		return x < 0.f ? -y : y;
	};
	outL = wantMix ? lim(mixL) : 0.f;
	outR = wantMix ? lim(mixR) : 0.f;
	cueL = lim(cuL);
	cueR = lim(cuR);
}

// ---------------------------------------------------------------------------------
// Intents (audio thread)
// ---------------------------------------------------------------------------------

// Any explicit press on a track ends its rolling recording (Ableton-style: user action
// wins — without this an auto-advance chain over a sustained input eats the column with
// no way out but the track STOP button). The in-flight cell is discarded;
// already-committed chain cells stay, and the chain is stamped closed. `except` skips
// one slot (a press ON the recording cell queues a FINISH instead — see pressSlot).
void LooperEngine::cancelRecording(int t, int except) {
	Track& tr = tracks[t];
	for (int s = 0; s < MAX_SLOTS; s++) {
		if (s == except) continue;
		Slot& sl = tr.slots[s];
		if (sl.state.load(std::memory_order_relaxed) == RECORDING) {
			sl.pending.store(NONE, std::memory_order_relaxed); // a queued FINISH dies with the recording
			discardRecording(t, s); // staging back to the worker; cell + UI remnants reset
			breakChain(tr, s);
		}
	}
}

void LooperEngine::pressSlot(int t, int s, bool overdubLatch) {
	if (t < 0 || t >= MAX_TRACKS || s < 0 || s >= MAX_SLOTS) return;
	Track& tr = tracks[t];
	Slot& sl = tr.slots[s];
	cancelRecording(t, s);
	// One queued action per instrument, latest press wins: a press disarms every OTHER
	// cell's pending on this track (same rule the scene commit applies across scenes).
	// Without this, launches could be armed on several cells of one track and the
	// beat would pick the highest slot index — not what was pressed last.
	for (int o = 0; o < MAX_SLOTS; o++)
		if (o != s) {
			Slot& other = tr.slots[o];
			if (other.pending.load(std::memory_order_relaxed) == CAPTURE)
				dropCapture(other); // a disarmed capture returns its staging
			other.pending.store(NONE, std::memory_order_relaxed);
		}
	if (sl.pending.load(std::memory_order_relaxed) != NONE) {
		// Press again = cancel (an armed capture returns its staging too).
		if (sl.pending.load(std::memory_order_relaxed) == CAPTURE)
			dropCapture(sl);
		sl.pending.store(NONE, std::memory_order_relaxed);
		return;
	}
	switch ((SlotState) sl.state.load(std::memory_order_relaxed)) {
		case EMPTY: {
			// Arm a capture: recording starts at the NEXT BEAT (mid-interval is the
			// point — the action grid is the beat). The staging buffer is requested
			// here so it is in hand by the beat (beatCommit retries if the grid
			// wasn't up yet); the sub-beat press→beat window records into its tail
			// as the pickup. dropCapture first: a stale staging left by an aborted
			// arm would otherwise be reused, folding its old pre-roll into this take.
			dropCapture(sl);
			for (int b = 0; b < THUMB_BINS; b++) sl.thumb[b] = 0.f;
			requestCaptureStaging(t, s);
			sl.pending.store(CAPTURE, std::memory_order_relaxed);
			break;
		}
		case RECORDING:
			// Finish, don't discard (Ableton: clicking a recording clip takes it): at
			// the next BEAT the take — whatever its length in beats — commits and
			// replays; a chain closes and cycles from its head. Pressing again cancels
			// the queued finish (the "press again = cancel" path above — the recording
			// rolls on); the track STOP button remains the discard.
			sl.pending.store(FINISH, std::memory_order_relaxed);
			break;
		case FILLED:
			sl.pending.store(LAUNCH, std::memory_order_relaxed);
			break;
		case PLAYING:
			// Overdub is a continuous, selection-driven mode (see tick): while the
			// OVERDUB latch is on, pressing a playing cell just selects it as the overdub
			// target (the module tracks selection) — it does not stop it. With the latch
			// off, a press stops the loop, as before.
			if (!overdubLatch)
				sl.pending.store(STOP, std::memory_order_relaxed);
			break;
	}
}

void LooperEngine::stopTrack(int t) {
	if (t < 0 || t >= MAX_TRACKS) return;
	cancelRecording(t, -1); // the stop button also disarms a rolling recording
	int p = tracks[t].playingSlot.load(std::memory_order_relaxed);
	if (p >= 0)
		tracks[t].slots[p].pending.store(STOP, std::memory_order_relaxed);
}

void LooperEngine::stopAll() {
	for (int t = 0; t < MAX_TRACKS; t++) stopTrack(t);
}

void LooperEngine::pressScene(int row) {
	if (row < 0 || row >= MAX_SLOTS) return;
	// Arming a scene disarms every launch queued outside its row (an earlier scene, a
	// single cell): only the latest scene fires at the beat. Same-thread as the
	// beat commit (audio), so clearing pendings here can't race it.
	for (int t = 0; t < MAX_TRACKS; t++)
		for (int s = 0; s < MAX_SLOTS; s++) {
			Slot& o = tracks[t].slots[s];
			if (s != row && o.pending.load(std::memory_order_relaxed) == LAUNCH)
				o.pending.store(NONE, std::memory_order_relaxed);
		}
	// The scene ACTS on a track (launch or stop): latest press wins there, exactly as a
	// cell press would — armed captures on other rows of that track disarm (with their
	// staging released), or their R_STEAL demote would land on the same beat as the
	// scene's launch and hard-cut its fade. Tracks the scene doesn't act on keep
	// everything.
	auto disarmOthers = [&](int t) {
		for (int o = 0; o < MAX_SLOTS; o++) {
			if (o == row) continue;
			Slot& oo = tracks[t].slots[o];
			if (oo.pending.load(std::memory_order_relaxed) == CAPTURE)
				dropCapture(oo);
			oo.pending.store(NONE, std::memory_order_relaxed);
		}
	};
	for (int t = 0; t < MAX_TRACKS; t++) {
		Slot& sl = tracks[t].slots[row];
		switch ((SlotState) sl.state.load(std::memory_order_relaxed)) {
			case EMPTY:
				// Ableton default: an empty slot in the scene stops the track — an
				// explicit stop, so it also disarms a rolling recording (stopTrack).
				disarmOthers(t);
				stopTrack(t);
				break;
			case FILLED:
				// Launching this track: the launch wins over a rolling recording on
				// some other row of the SAME track. Tracks the scene doesn't act on
				// keep their recordings — a scene is a launch gesture, not a global
				// disarm (a recording 45 s deep on another track must survive).
				disarmOthers(t);
				cancelRecording(t, -1);
				sl.pending.store(LAUNCH, std::memory_order_relaxed);
				break;
			case PLAYING:    // already what the scene wants
			case RECORDING:  // the scene points AT the recording: let it finish — it
				break;       // commits at the cap and starts playing by itself
		}
	}
}

// ---------------------------------------------------------------------------------
// UI thread
// ---------------------------------------------------------------------------------

void LooperEngine::requestClear(int t, int s) {
	Intent i {};
	i.kind = Intent::CLEAR; i.track = t; i.slot = s;
	intents.push(i);
}

void LooperEngine::requestClearAll() {
	Intent i {};
	i.kind = Intent::CLEAR_ALL; i.track = 0; i.slot = 0;
	intents.push(i);
}

void LooperEngine::requestCarrySpans() {
	Intent i {};
	i.kind = Intent::CARRY_SPANS; i.track = 0; i.slot = 0;
	intents.push(i);
}

int LooperEngine::pendingCount() const {
	int n = 0;
	for (int t = 0; t < MAX_TRACKS; t++)
		for (int s = 0; s < MAX_SLOTS; s++)
			if (tracks[t].slots[s].pending.load(std::memory_order_relaxed) != NONE) n++;
	return n;
}

} // namespace looper
} // namespace akaudio
