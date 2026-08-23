// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "LooperEngine.hpp"
#include "LooperWorker.hpp"

#include <algorithm>
#include <cmath>

namespace akaudio {
namespace looper {

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
		freeBuf(tr.rec); freeBuf(tr.last); freeBuf(tr.spare);
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
	Cmd c;
	c.kind = Cmd::RELEASE; c.track = c.slot = c.frames = c.upto = 0; c.seq = 0; c.a = b; c.b = nullptr;
	cmds.push(c); // a full queue would leak the buffer rather than block — sized so it can't happen
}

void LooperEngine::requestRec(int t) {
	Track& tr = tracks[t];
	// Rolling buffers follow the cable: an unpatched track holds none (at 32 BPI a
	// pair is 12-24 MB), a patched one gets its pair within a few ms of plugging in.
	if (tr.recPending || N <= 0 || !tr.present.load(std::memory_order_relaxed))
		return;
	Cmd c;
	c.kind = Cmd::ALLOC; c.track = t; c.slot = 0; c.frames = N; c.upto = 0; c.seq = seqCounter++; c.a = c.b = nullptr;
	if (cmds.push(c))
		tr.recPending = true;
}

// Hand a freshly committed take to the sink for encoding + indexing (M4). Runs on the
// audio thread but only enqueues a POD — the encode happens on the worker. The take's
// buffer stays alive for the save because its RELEASE (if it is ever overwritten) is
// pushed to the SAME queue afterwards and the worker serves the queue in order.
void LooperEngine::saveTake(int t, int s) {
	if (!sink) return;
	Slot& sl = tracks[t].slots[s];
	if (!sl.take.buf || sl.take.frames != N) return;
	Cmd c;
	c.kind = Cmd::SAVE; c.track = t; c.slot = s; c.frames = N; c.upto = 0;
	c.seq = seqCounter++; c.a = sl.take.buf; c.b = nullptr;
	c.meta.frames = sl.take.frames;
	c.meta.sampleRate = sl.take.sampleRate;
	c.meta.bpm = sl.take.bpm; c.meta.bpi = sl.take.bpi;
	c.meta.startFrame = sl.take.startFrame;
	c.meta.peak = sl.take.peak;
	c.meta.repeats = sl.repeats.load(std::memory_order_relaxed);
	c.meta.decayDb = sl.decayDb.load(std::memory_order_relaxed);
	cmds.push(c); // a dropped save just misses the disk copy; never blocks the audio thread
}

// Enqueue "retire this slot's live file into history/" (M4). The buffer is released
// separately; this only touches the on-disk file + manifest, on the worker.
void LooperEngine::clearFile(int t, int s) {
	if (!sink) return;
	Cmd c;
	c.kind = Cmd::CLEAR_FILE; c.track = t; c.slot = s; c.frames = 0; c.upto = 0;
	c.seq = 0; c.a = c.b = nullptr;
	cmds.push(c);
}

// Arm a continuous overdub on (t,s) for the interval that is about to begin: ask the
// worker for staging = a copy of the current take (upto=0 — we fold the whole interval as
// it records). Returns true if the request was issued. The audio thread folds the rolling
// buffer into staging as it records (see tick), and the boundary commits it.
bool LooperEngine::armOverdub(int t, int s) {
	Track& tr = tracks[t];
	Slot& sl = tr.slots[s];
	if (!tr.rec || !sl.take.buf || sl.take.frames != N)
		return false;
	Cmd cmd;
	cmd.kind = Cmd::OVERDUB_COPY; cmd.track = t; cmd.slot = s; cmd.frames = N; cmd.upto = 0;
	cmd.seq = seqCounter++; cmd.a = sl.take.buf; cmd.b = tr.rec;
	if (!cmds.push(cmd))
		return false;
	sl.odSeq = cmd.seq;
	sl.odCatch = 0;
	sl.odReady = false;
	return true;
}

void LooperEngine::dropOverdub(Slot& sl) {
	if (sl.staging) {
		release(sl.staging);
		sl.staging = nullptr;
	}
	sl.odReady = false;
	sl.odSeq = 0;
	sl.odCatch = 0;
}

void LooperEngine::setPlaying(Track& tr, int s) {
	int prev = tr.playingSlot.load(std::memory_order_relaxed);
	if (prev >= 0 && prev != s)
		tr.slots[prev].state.store(FILLED, std::memory_order_relaxed);
	Slot& sl = tr.slots[s];
	sl.state.store(PLAYING, std::memory_order_relaxed);
	sl.repCount.store(0, std::memory_order_relaxed);
	sl.gain.store(1.f, std::memory_order_relaxed);
	sl.startedThisBoundary = true;
	tr.playingSlot.store(s, std::memory_order_relaxed);
}

void LooperEngine::drainReplies() {
	Reply r;
	while (replies.pop(r)) {
		if (r.track < 0 || r.track >= MAX_TRACKS) { release(r.buf); continue; }
		Track& tr = tracks[r.track];
		switch (r.kind) {
			case Reply::ALLOC:
				tr.recPending = false;
				if (!r.buf) break;
				if (r.buf->frames != N)      release(r.buf); // grid moved meanwhile
				else if (!tr.rec)            tr.rec = r.buf;
				else if (!tr.spare)          tr.spare = r.buf;
				else                         release(r.buf);
				if (!tr.rec || (!tr.last && !tr.spare))
					requestRec(r.track); // top up the rolling pair right away
				break;
			case Reply::OVERDUB_COPY: {
				Slot& sl = tr.slots[r.slot];
				// Accept the staging only if it answers the current arm (odSeq) and fits the
				// grid; a superseded / cancelled / regridded request is recycled.
				if (sl.odSeq == r.seq && sl.odSeq != 0 && r.buf && r.buf->frames == N) {
					sl.staging = r.buf;
					sl.odReady = true;
				} else {
					release(r.buf);
				}
				break;
			}
		}
		tr.bufs.store((tr.rec ? 1 : 0) + (tr.last ? 1 : 0) + (tr.spare ? 1 : 0), std::memory_order_relaxed);
	}
}

void LooperEngine::drainIntents() {
	Intent i;
	while (intents.pop(i)) {
		if (i.track < 0 || i.track >= MAX_TRACKS || i.slot < 0 || i.slot >= MAX_SLOTS) continue;
		Track& tr = tracks[i.track];
		Slot& sl = tr.slots[i.slot];
		switch (i.kind) {
			case Intent::CLEAR: {
				sl.pending.store(NONE, std::memory_order_relaxed);
				dropOverdub(sl);
				sl.overdubbing.store(false, std::memory_order_relaxed);
				if (odTrack == i.track && odSlot == i.slot) { odTrack = odSlot = -1; }
				if (tr.playingSlot.load(std::memory_order_relaxed) == i.slot)
					tr.playingSlot.store(-1, std::memory_order_relaxed);
				const bool hadTake = sl.take.buf != nullptr;
				if (sl.take.buf) { release(sl.take.buf); sl.take = Take(); }
				if (hadTake) clearFile(i.track, i.slot); // retire the live file into history/ (M4)
				sl.state.store(EMPTY, std::memory_order_relaxed);
				sl.playable.store(true, std::memory_order_relaxed);
				for (int b = 0; b < THUMB_BINS; b++) sl.thumb[b] = 0.f;
				break;
			}
		}
	}
}

// Install decoded takes handed over by the worker (clip loader, v2). Each becomes a
// FILLED slot with its saved settings; playability is decided against the live grid (a
// take whose length/rate matches the current N is launchable, else greyed until it does —
// the regrid rule). Buffer ownership passes to the slot (freed on clear/overwrite).
void LooperEngine::drainLoads() {
	LoadInstall li;
	while (loads.pop(li)) {
		if (li.track < 0 || li.track >= MAX_TRACKS || li.slot < 0 || li.slot >= MAX_SLOTS) {
			release(li.buf);
			continue;
		}
		Slot& sl = tracks[li.track].slots[li.slot];
		if (sl.take.buf) release(sl.take.buf); // replace whatever is there
		sl.take.buf = li.buf;
		sl.take.frames = li.buf->frames; // the buffer's real length (== declared N when set right)
		sl.take.bpm = li.meta.bpm;
		sl.take.bpi = li.meta.bpi;
		sl.take.sampleRate = li.meta.sampleRate;
		sl.take.startFrame = li.meta.startFrame;
		sl.take.peak = li.meta.peak;
		sl.repeats.store(li.meta.repeats, std::memory_order_relaxed);
		sl.decayDb.store(li.meta.decayDb, std::memory_order_relaxed);
		for (int b = 0; b < THUMB_BINS; b++) sl.thumb[b] = li.thumb[b];
		bool ok = haveGrid && li.buf && li.buf->frames == N && li.meta.sampleRate == sr;
		sl.playable.store(!haveGrid || ok, std::memory_order_relaxed);
		sl.repCount.store(0, std::memory_order_relaxed);
		sl.gain.store(1.f, std::memory_order_relaxed);
		sl.pending.store(NONE, std::memory_order_relaxed);
		sl.state.store(FILLED, std::memory_order_relaxed);
	}
}

// The grid moved (first clock, join, tempo change, sample rate): queued actions were
// aimed at the old grid — cancel them; rolling buffers of the wrong length go back to
// the worker; takes of another length stay but aren't launchable; a playing take of
// the right length keeps playing (re-join at the same tempo — its playhead follows
// the new frame 0).
void LooperEngine::regrid(const ClockFrame& c) {
	N = c.intervalFrames;
	sr = c.sampleRate;
	gen = c.gridGeneration;
	haveGrid = true;
	intervalFrames.store(N, std::memory_order_relaxed);
	gateDecay = sr > 0.f ? std::exp(-1.f / (0.1f * sr)) : 0.f; // ~100 ms release
	declickN = N > 0 ? std::max(1, std::min(N / 4, (int) (sr * 0.0015f))) : 0; // ~1.5 ms loop-end fade
	for (int t = 0; t < MAX_TRACKS; t++) {
		Track& tr = tracks[t];
		auto drop = [&](Buf*& b) { if (b && b->frames != N) { release(b); b = nullptr; } };
		drop(tr.rec); drop(tr.last); drop(tr.spare);
		tr.peak = 0.f;
		for (int b = 0; b < THUMB_BINS; b++) tr.live[b] = 0.f;
		for (int s = 0; s < MAX_SLOTS; s++) {
			Slot& sl = tr.slots[s];
			sl.pending.store(NONE, std::memory_order_relaxed);
			dropOverdub(sl);
			sl.overdubbing.store(false, std::memory_order_relaxed);
			if (sl.state.load(std::memory_order_relaxed) == RECORDING)
				sl.state.store(EMPTY, std::memory_order_relaxed);
			bool ok = sl.take.buf && sl.take.frames == N && sl.take.sampleRate == sr;
			sl.playable.store(ok || !sl.take.buf, std::memory_order_relaxed);
			if (sl.state.load(std::memory_order_relaxed) == PLAYING && !ok) {
				sl.state.store(FILLED, std::memory_order_relaxed);
				if (tr.playingSlot.load(std::memory_order_relaxed) == s)
					tr.playingSlot.store(-1, std::memory_order_relaxed);
			}
		}
		if (!tr.rec || (!tr.last && !tr.spare))
			requestRec(t);
		tr.bufs.store((tr.rec ? 1 : 0) + (tr.last ? 1 : 0) + (tr.spare ? 1 : 0), std::memory_order_relaxed);
	}
	odTrack = odSlot = -1; // the overdub re-targets against the new grid at the next boundary
}

// Interval boundary (this frame is frame 0 of the next interval). Per track, in order:
// finish overdub accumulation, rotate the rolling buffers, commit pending actions,
// wrap the playing slot (repeats / decay).
void LooperEngine::boundary(const ClockFrame& c, double now) {
	for (int t = 0; t < MAX_TRACKS; t++) {
		Track& tr = tracks[t];
		const bool present = tr.present.load(std::memory_order_relaxed);

		// 0. The active overdub whose staging arrived late in the interval: add the
		// remaining frames of the rolling buffer (bounded by the worker's latency, ~ms).
		if (odTrack == t && odSlot >= 0) {
			Slot& sl = tr.slots[odSlot];
			if (sl.odReady && sl.staging && tr.rec) {
				for (; sl.odCatch < N; sl.odCatch++) {
					sl.staging->pcm[(size_t) sl.odCatch * 2]     += tr.rec->pcm[(size_t) sl.odCatch * 2];
					sl.staging->pcm[(size_t) sl.odCatch * 2 + 1] += tr.rec->pcm[(size_t) sl.odCatch * 2 + 1];
				}
			}
		}

		// 1. Rotate: the recording buffer becomes `last` (the completed interval); the
		// previous `last` (or the spare) records next. Pointer moves only.
		Buf* done = tr.rec;
		if (tr.last)       { tr.rec = tr.last; tr.last = nullptr; }
		else if (tr.spare) { tr.rec = tr.spare; tr.spare = nullptr; }
		else               tr.rec = nullptr;
		tr.last = done;
		tr.lastPeak = tr.peak;
		tr.peak = 0.f;
		for (int b = 0; b < THUMB_BINS; b++) { tr.lastLive[b] = tr.live[b]; tr.live[b] = 0.f; }
		const bool lastOk = tr.last != nullptr && tr.last->frames == N;

		// 2a. A slot that was RECORDING this interval: the completed interval becomes its
		// take (`last` moves into the slot, no copy) and it starts playing. Refused —
		// back to Empty — if nothing was recorded (cable gone, no buffer yet) or it was
		// silence (−70 dBFS gate).
		for (int s = 0; s < MAX_SLOTS; s++) {
			Slot& sl = tr.slots[s];
			if (sl.state.load(std::memory_order_relaxed) != RECORDING) continue;
			if (!present || !lastOk || tr.lastPeak < GATE) {
				refuse(sl, now);
				sl.state.store(EMPTY, std::memory_order_relaxed);
				continue;
			}
			if (sl.take.buf) release(sl.take.buf);
			sl.take.buf = tr.last;
			sl.take.frames = N;
			sl.take.bpm = c.bpm; sl.take.bpi = c.bpi; sl.take.sampleRate = sr;
			sl.take.startFrame = c.sessionFrame >= (uint64_t) N ? c.sessionFrame - (uint64_t) N : 0;
			sl.take.peak = tr.lastPeak;
			tr.last = nullptr;
			for (int b = 0; b < THUMB_BINS; b++) sl.thumb[b] = tr.lastLive[b];
			sl.repeats.store(defRepeats.load(std::memory_order_relaxed), std::memory_order_relaxed);
			sl.decayDb.store(defDecayDb.load(std::memory_order_relaxed), std::memory_order_relaxed);
			sl.playable.store(true, std::memory_order_relaxed);
			setPlaying(tr, s);
			saveTake(t, s); // encode + index the captured interval (M4)
		}

		// 2b. Commit pending actions.
		for (int s = 0; s < MAX_SLOTS; s++) {
			Slot& sl = tr.slots[s];
			int p = sl.pending.exchange(NONE, std::memory_order_relaxed);
			switch (p) {
				case CAPTURE: {
					// Start recording the interval that begins now (Ableton clip semantics);
					// the take is committed at the next boundary. One recording slot per
					// track: arming another cancels the previous one.
					if (!present || !tr.rec) { refuse(sl, now); break; }
					for (int o = 0; o < MAX_SLOTS; o++)
						if (o != s && tr.slots[o].state.load(std::memory_order_relaxed) == RECORDING)
							tr.slots[o].state.store(EMPTY, std::memory_order_relaxed);
					sl.state.store(RECORDING, std::memory_order_relaxed);
					break;
				}
				case LAUNCH:
					if (!sl.take.buf || sl.take.frames != N) { refuse(sl, now); break; }
					setPlaying(tr, s);
					break;
				case STOP:
					if (sl.state.load(std::memory_order_relaxed) == PLAYING) {
						sl.state.store(FILLED, std::memory_order_relaxed);
						if (tr.playingSlot.load(std::memory_order_relaxed) == s)
							tr.playingSlot.store(-1, std::memory_order_relaxed);
					}
					break;
				default:
					break;
			}
		}

		// 2c. Commit the active continuous overdub on this track: staging (take + the
		// interval that just ended) becomes the new take — the playhead / repeat counter
		// keep running, so the loop layers without restarting. Dropped if it stopped
		// playing this boundary (e.g. STOP) or its staging never arrived.
		if (odTrack == t && odSlot >= 0) {
			Slot& sl = tr.slots[odSlot];
			if (sl.state.load(std::memory_order_relaxed) == PLAYING && sl.odReady && sl.staging) {
				if (sl.take.buf) release(sl.take.buf);
				sl.take.buf = sl.staging;
				sl.take.frames = N;
				sl.take.peak = std::max(sl.take.peak, tr.lastPeak);
				sl.staging = nullptr;
				sl.odReady = false;
				sl.odSeq = 0;
				for (int b = 0; b < THUMB_BINS; b++) sl.thumb[b] = std::min(1.f, std::max(sl.thumb[b], tr.lastLive[b]));
				saveTake(t, odSlot); // the overdubbed take replaces the file (old → history), M4
			} else {
				dropOverdub(sl);
			}
		}

		// 3. The playing slot wraps: repeats + decay (not on the boundary it started on).
		int ps = tr.playingSlot.load(std::memory_order_relaxed);
		if (ps >= 0) {
			Slot& sl = tr.slots[ps];
			if (sl.startedThisBoundary) {
				sl.startedThisBoundary = false;
			} else {
				int rc = sl.repCount.load(std::memory_order_relaxed) + 1;
				sl.repCount.store(rc, std::memory_order_relaxed);
				float g = std::pow(10.f, sl.decayDb.load(std::memory_order_relaxed) * (float) rc / 20.f);
				sl.gain.store(g, std::memory_order_relaxed);
				int reps = sl.repeats.load(std::memory_order_relaxed);
				if ((reps > 0 && rc >= reps) || g < 1e-3f) {
					sl.state.store(FILLED, std::memory_order_relaxed);
					tr.playingSlot.store(-1, std::memory_order_relaxed);
				}
			}
		}

		// Keep the rolling set stocked while a cable is present: we need `rec` now and a
		// replacement for the next rotation (`last` or `spare`). An unpatched track
		// hands its buffers back.
		if (!present) {
			if (tr.rec)   { release(tr.rec);   tr.rec = nullptr; }
			if (tr.last)  { release(tr.last);  tr.last = nullptr; }
			if (tr.spare) { release(tr.spare); tr.spare = nullptr; }
		} else if (!tr.rec || (!tr.last && !tr.spare)) {
			requestRec(t);
		}
		tr.bufs.store((tr.rec ? 1 : 0) + (tr.last ? 1 : 0) + (tr.spare ? 1 : 0), std::memory_order_relaxed);
	}

	// Re-target the continuous overdub for the interval that just began: while the OVERDUB
	// latch is on, the selected playing (patched) cell overdubs each interval; changing the
	// selection (or disengaging the latch) moves/stops it here, at the boundary. The
	// previous target committed above (2c), so this arms fresh on the current take.
	if (odTrack >= 0 && odSlot >= 0)
		tracks[odTrack].slots[odSlot].overdubbing.store(false, std::memory_order_relaxed);
	odTrack = odSlot = -1;
	if (overdubMode.load(std::memory_order_relaxed)) {
		int sel = overdubSel.load(std::memory_order_relaxed);
		if (sel >= 0 && sel < MAX_TRACKS * MAX_SLOTS) {
			int st = sel / MAX_SLOTS, ss = sel % MAX_SLOTS;
			Track& tr = tracks[st];
			Slot& sl = tr.slots[ss];
			if (tr.present.load(std::memory_order_relaxed)
			        && sl.state.load(std::memory_order_relaxed) == PLAYING
			        && sl.take.buf && sl.take.frames == N
			        && armOverdub(st, ss)) {
				odTrack = st;
				odSlot = ss;
				sl.overdubbing.store(true, std::memory_order_relaxed);
			}
		}
	}
}

// ---------------------------------------------------------------------------------
// Per-frame
// ---------------------------------------------------------------------------------

void LooperEngine::tick(const ClockFrame& c, const TrackIn* in, int nTracks, double now,
                        float& outL, float& outR, float& cueL, float& cueR, float* trackOutLR,
                        bool wantMix) {
	drainReplies();
	drainIntents();
	drainLoads();
	if (c.running && c.intervalFrames > 0) {
		if (!haveGrid || c.gridGeneration != gen || c.intervalFrames != N || c.sampleRate != sr)
			regrid(c);
		if (c.downbeat) {
			boundary(c, now);
			lastFrameRecorded = -1;
		}
	}
	const bool grid = haveGrid && N > 0 && c.running;
	const int f = c.frameInInterval;
	const bool inGrid = grid && f >= 0 && f < N;
	const int bin = inGrid ? (int) std::min<long long>(THUMB_BINS - 1, (long long) f * THUMB_BINS / N) : 0;
	if (inGrid)
		lastFrameRecorded = f;

	const bool declick = declickEnabled.load(std::memory_order_relaxed) && declickN > 0;

	float mixL = 0.f, mixR = 0.f, cuL = 0.f, cuR = 0.f;
	for (int t = 0; t < MAX_TRACKS; t++) {
		Track& tr = tracks[t];
		const bool has = t < nTracks;
		const bool present = has && in[t].present;
		tr.present.store(present, std::memory_order_relaxed);
		if (present && grid && (!tr.rec || (!tr.last && !tr.spare)))
			requestRec(t); // no-op while a request is in flight
		const float l = present ? in[t].l : 0.f;
		const float r = present ? in[t].r : 0.f;
		const float a = std::max(std::fabs(l), std::fabs(r));

		// Always-record (+ the live thumbnail an armed slot draws).
		if (inGrid) {
			if (a > tr.live[bin]) tr.live[bin] = std::min(1.f, a);
			if (a > tr.peak) tr.peak = a;
			if (tr.rec) {
				tr.rec->pcm[(size_t) f * 2] = l;
				tr.rec->pcm[(size_t) f * 2 + 1] = r;
				// Overdub in progress: fold the rolling buffer into staging as we go, catching
				// up (≤256 frames per tick) on frames recorded before the copy arrived.
				for (int s = 0; s < MAX_SLOTS; s++) {
					Slot& sl = tr.slots[s];
					if (!sl.odReady || !sl.staging) continue;
					int steps = 0;
					while (sl.odCatch <= f && steps++ < 256) {
						sl.staging->pcm[(size_t) sl.odCatch * 2]     += tr.rec->pcm[(size_t) sl.odCatch * 2];
						sl.staging->pcm[(size_t) sl.odCatch * 2 + 1] += tr.rec->pcm[(size_t) sl.odCatch * 2 + 1];
						sl.odCatch++;
					}
				}
			}
		}

		// Loop playback: the playing slot at this frame of the interval.
		float loopL = 0.f, loopR = 0.f;
		int ps = tr.playingSlot.load(std::memory_order_relaxed);
		if (ps >= 0 && inGrid) {
			Slot& sl = tr.slots[ps];
			if (sl.take.buf && f < sl.take.frames) {
				float g = sl.gain.load(std::memory_order_relaxed);
				// Declick: fade the first/last declickN frames of the cycle smoothly to 0, so
				// the loop point (frame N-1 → 0) is continuous through zero — no click. The
				// smoothstep has zero slope at the ends, so even a very short fade is clean.
				if (declick) {
					const int nf = sl.take.frames;
					float ft = 1.f;
					if (f < declickN)              ft = (f + 0.5f) / declickN;
					else if (f >= nf - declickN)   ft = (nf - f - 0.5f) / declickN;
					if (ft < 1.f) g *= ft * ft * (3.f - 2.f * ft);
				}
				loopL = sl.take.buf->pcm[(size_t) f * 2] * g;
				loopR = sl.take.buf->pcm[(size_t) f * 2 + 1] * g;
			}
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

void LooperEngine::pressSlot(int t, int s, bool overdubMode) {
	if (t < 0 || t >= MAX_TRACKS || s < 0 || s >= MAX_SLOTS) return;
	Track& tr = tracks[t];
	Slot& sl = tr.slots[s];
	if (sl.pending.load(std::memory_order_relaxed) != NONE) {
		sl.pending.store(NONE, std::memory_order_relaxed); // press again = cancel
		dropOverdub(sl);
		return;
	}
	switch (sl.state.load(std::memory_order_relaxed)) {
		case EMPTY:
			sl.pending.store(CAPTURE, std::memory_order_relaxed);
			break;
		case RECORDING:
			sl.state.store(EMPTY, std::memory_order_relaxed); // cancel the recording
			break;
		case FILLED:
			sl.pending.store(LAUNCH, std::memory_order_relaxed);
			break;
		case PLAYING:
			// Overdub is a continuous, selection-driven mode (see boundary): while the
			// OVERDUB latch is on, pressing a playing cell just selects it as the overdub
			// target (the module tracks selection) — it does not stop it. With the latch
			// off, a press stops the loop, as before.
			if (!overdubMode)
				sl.pending.store(STOP, std::memory_order_relaxed);
			break;
	}
}

void LooperEngine::stopTrack(int t) {
	if (t < 0 || t >= MAX_TRACKS) return;
	int p = tracks[t].playingSlot.load(std::memory_order_relaxed);
	if (p >= 0)
		tracks[t].slots[p].pending.store(STOP, std::memory_order_relaxed);
}

void LooperEngine::stopAll() {
	for (int t = 0; t < MAX_TRACKS; t++) stopTrack(t);
}

void LooperEngine::pressScene(int row) {
	if (row < 0 || row >= MAX_SLOTS) return;
	for (int t = 0; t < MAX_TRACKS; t++) {
		Slot& sl = tracks[t].slots[row];
		switch (sl.state.load(std::memory_order_relaxed)) {
			case EMPTY:     stopTrack(t); break; // Ableton default: an empty slot in the scene stops the track
			case FILLED:    sl.pending.store(LAUNCH, std::memory_order_relaxed); break;
			case PLAYING:   break;
			case RECORDING: break; // let it finish
		}
	}
}

// ---------------------------------------------------------------------------------
// UI thread
// ---------------------------------------------------------------------------------

void LooperEngine::requestClear(int t, int s) {
	Intent i;
	i.kind = Intent::CLEAR; i.track = t; i.slot = s;
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
