// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "LooperWorker.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace akaudio {
namespace looper {

// Peak-per-bin thumbnail of a decoded take (display only), same shape the capture path
// fills so a restored clip draws its waveform.
static void computeThumb(const float* pcm, int frames, float* thumb) {
	for (int b = 0; b < THUMB_BINS; b++) thumb[b] = 0.f;
	if (frames <= 0) return;
	for (int f = 0; f < frames; f++) {
		int bin = (int) ((long long) f * THUMB_BINS / frames);
		if (bin >= THUMB_BINS) bin = THUMB_BINS - 1;
		float a = std::max(std::fabs(pcm[(size_t) f * 2]), std::fabs(pcm[(size_t) f * 2 + 1]));
		if (a > thumb[bin]) thumb[bin] = std::min(1.f, a);
	}
}

LooperWorker::~LooperWorker() {
	stop();
}

void LooperWorker::start() {
	if (thread.joinable())
		return;
	quit.store(false, std::memory_order_relaxed);
	thread = std::thread(&LooperWorker::run, this);
}

void LooperWorker::stop() {
	quit.store(true, std::memory_order_release);
	if (thread.joinable())
		thread.join();
	for (Buf* b : pool) {
		delete[] b->pcm;
		delete b;
	}
	pool.clear();
}

Buf* LooperWorker::alloc(int frames) {
	for (size_t i = 0; i < pool.size(); i++) {
		if (pool[i]->frames == frames) {
			Buf* b = pool[i];
			pool[i] = pool.back();
			pool.pop_back();
			return b;
		}
	}
	Buf* b = new Buf;
	b->frames = frames;
	b->pcm = new float[(size_t) frames * 2];
	engine.allocations.fetch_add(1, std::memory_order_relaxed);
	return b;
}

void LooperWorker::recycle(Buf* b) {
	if (!b)
		return;
	// Keep a few buffers of the live interval length; anything else (old grid) goes.
	int n = engine.intervalFrames.load(std::memory_order_relaxed);
	if (b->frames == n && pool.size() < POOL_MAX) {
		pool.push_back(b);
		return;
	}
	delete[] b->pcm;
	delete b;
}

// Windowed-sinc varispeed for BPM conversion (worker thread; allocation-free, the
// caller provides the output). Maps `srcN` interleaved-stereo frames onto `outN`
// (speed changes by srcN/outN, pitch with it — tape-style). 16 taps per side with a
// Hann window and an anti-alias cutoff when slowing down; per-sample normalization
// keeps unity gain at the take's edges. Quality is deliberately "small, deterministic,
// dependency-free" — well above the audibility bar for re-pitching loop material.
static void resampleSinc(const float* src, int srcN, float* out, int outN) {
	const int TAPS = 16;
	const double step = (double) srcN / outN;
	const double cut = outN < srcN ? (double) outN / srcN : 1.0; // anti-alias when slowing the rate down
	for (int i = 0; i < outN; i++) {
		double pos = (i + 0.5) * step - 0.5;
		int c = (int) std::floor(pos);
		double frac = pos - c;
		double sumL = 0.0, sumR = 0.0, wsum = 0.0;
		for (int k = -TAPS + 1; k <= TAPS; k++) {
			int idx = c + k;
			if (idx < 0 || idx >= srcN) continue;
			double x = k - frac;
			double px = M_PI * x * cut;
			double sinc = px == 0.0 ? 1.0 : std::sin(px) / px;
			double w = cut * sinc * (0.5 + 0.5 * std::cos(M_PI * x / TAPS)); // Hann over ±TAPS
			sumL += w * src[(size_t) idx * 2];
			sumR += w * src[(size_t) idx * 2 + 1];
			wsum += w;
		}
		double g = wsum != 0.0 ? 1.0 / wsum : 0.0;
		out[(size_t) i * 2] = (float) (sumL * g);
		out[(size_t) i * 2 + 1] = (float) (sumR * g);
	}
}

void LooperWorker::run() {
	Cmd c;
	while (!quit.load(std::memory_order_acquire)) {
		bool did = false;
		while (engine.cmds.pop(c)) {
			did = true;
			switch (c.kind) {
				case Cmd::ALLOC: {
					Buf* b = alloc(c.frames);
					std::memset(b->pcm, 0, sizeof(float) * (size_t) b->frames * 2);
					Reply r {};
					r.kind = Reply::ALLOC; r.track = c.track; r.slot = c.slot; r.seq = c.seq; r.buf = b;
					if (!engine.replies.push(r))
						recycle(b);
					break;
				}
				case Cmd::OVERDUB_COPY: {
					// staging = take, plus the part of the current interval already recorded
					// before the press ([0, upto) of the rolling buffer — frames the audio
					// thread has finished writing; it adds the rest itself as it records).
					Buf* b = alloc(c.frames);
					const size_t n2 = (size_t) c.frames * 2;
					if (c.a && c.a->frames == c.frames)
						std::memcpy(b->pcm, c.a->pcm, sizeof(float) * n2);
					else
						std::memset(b->pcm, 0, sizeof(float) * n2);
					if (c.b && c.b->frames == c.frames) {
						const size_t u2 = (size_t) (c.upto < c.frames ? c.upto : c.frames) * 2;
						for (size_t i = 0; i < u2; i++)
							b->pcm[i] += c.b->pcm[i];
					}
					Reply r {};
					r.kind = Reply::OVERDUB_COPY; r.track = c.track; r.slot = c.slot; r.seq = c.seq; r.buf = b;
					if (!engine.replies.push(r))
						recycle(b);
					break;
				}
				case Cmd::RELEASE:
					recycle(c.a);
					break;
				case Cmd::SAVE:
					// Encode + index a committed take (M4). The buffer is read-only and stays
					// valid: its RELEASE, if any, is queued after this. Slow (OGG encode + I/O),
					// but takes commit at most a few per interval and intervals are seconds long.
					if (engine.sink && c.a)
						engine.sink->save(c.track, c.slot, c.a->pcm, c.meta);
					break;
				case Cmd::CLEAR_FILE:
					if (engine.sink)
						engine.sink->clear(c.track, c.slot);
					break;
				case Cmd::EVENT:
					// Performance event → events.jsonl. FIFO with SAVE means the take a
					// START references has already reached the sink.
					if (engine.sink)
						engine.sink->event(c.ev);
					break;
				case Cmd::CONVERT: {
					// Tempo conversion (BPI halved/doubled, and/or BPM changed within
					// 0.5×–2×): derive grid-fitting takes. The source (c.a) is an
					// immutable committed take, valid like a SAVE's (any RELEASE is
					// queued after this command). Results go through the guarded load
					// path: a stale result is recycled by the audio thread, never
					// installed. RAM-only — nothing is saved to disk; the session
					// keeps the original take + settings.
					if (!c.a || c.frames <= 1 || c.a->frames <= 0) break;
					const int n = c.frames;
					const int srcN = c.a->frames;
					// Placement mode (c.seq): 0 = in-place, 1 = tile ×2, 2 = split.
					// First fit the source to L frames: an exact copy when lengths
					// agree (pure BPI change — keeps tiling/splitting sample-exact),
					// pad/trim a rounding drift, else windowed-sinc varispeed (a BPM
					// change re-pitches, tape-style). By construction L/srcN is
					// exactly the tempo ratio, so the grid does the math for us.
					const int L = c.seq == 1 ? n / 2 : c.seq == 2 ? 2 * n : n; // ≥ 1: n > 1 above
					std::vector<float> fit((size_t) L * 2, 0.f);
					if (srcN == L) {
						std::memcpy(fit.data(), c.a->pcm, sizeof(float) * (size_t) L * 2);
					} else if (std::abs(srcN - L) <= 4) {
						const int m = std::min(srcN, L);
						std::memcpy(fit.data(), c.a->pcm, sizeof(float) * (size_t) m * 2);
					} else {
						resampleSinc(c.a->pcm, srcN, fit.data(), L);
					}
					auto push = [&](int slot, Buf* expect, int srcFrom, const TakeMeta& m) {
						Buf* b = alloc(n);
						for (int f = 0; f < n; f++) {
							int sf = srcFrom < 0 ? f % L : srcFrom + f; // tile vs slice
							bool in = sf < L;
							b->pcm[(size_t) f * 2]     = in ? fit[(size_t) sf * 2] : 0.f;
							b->pcm[(size_t) f * 2 + 1] = in ? fit[(size_t) sf * 2 + 1] : 0.f;
						}
						LoadInstall li {};
						li.track = c.track; li.slot = slot; li.buf = b; li.meta = m;
						li.guard = true; li.expect = expect; li.derived = true;
						computeThumb(b->pcm, n, li.thumb);
						if (!engine.submitLoad(li))
							recycle(b);
					};
					if (c.seq == 0) {
						// In-place varispeed: same bar count, new tempo.
						push(c.slot, c.a, 0, c.meta);
					} else if (c.seq == 1) {
						// Tile ×2: the take plays twice per doubled interval — exactly
						// what the room heard. Settings carry over unchanged.
						push(c.slot, c.a, -1, c.meta);
					} else {
						// Split into two ×1-chained halves: the pair replays the take
						// exactly. The second half takes the original's After; an
						// endless original (repeats ∞, After Stop) becomes an A↔B
						// cycle. (A finite repeat count can't span a chain — the pair
						// plays once, then the original's After. Decay resets on each
						// chain hop, so it is effectively lost — as in any chain.)
						TakeMeta m1 = c.meta;
						m1.repeats = 1;
						m1.followSlot = c.upto + 1; // 1-based: → the second half
						TakeMeta m2 = c.meta;
						m2.repeats = 1;
						m2.startFrame = c.meta.startFrame + (uint64_t) n;
						if (m2.followSlot == 0 && c.meta.repeats == 0)
							m2.followSlot = c.slot + 1; // 1-based: back to the first half
						push(c.slot, c.a, 0, m1);
						push(c.upto, nullptr, n, m2);
					}
					break;
				}
			}
		}
		if (engine.sink)
			engine.sink->flush(); // write the manifest if UI edits (names / settings) dirtied it

		// Clip loader (v2): decode any pending takes off the audio thread and hand them to
		// the engine to install. We allocate the buffer here (the buffer-owning thread).
		if (engine.sink) {
			int t, s, frames;
			std::vector<float> pcm;
			TakeMeta meta;
			while (engine.sink->nextLoad(t, s, pcm, frames, meta)) {
				did = true;
				// Empty pcm = the OGG was missing or undecodable: skip, so the slot
				// stays EMPTY instead of installing an all-zero silent take.
				if (frames <= 0 || pcm.empty()) continue;
				Buf* b = alloc(frames);
				const size_t need = (size_t) frames * 2;
				const size_t have = std::min(pcm.size(), need);
				std::memcpy(b->pcm, pcm.data(), have * sizeof(float));
				if (have < need)
					std::memset(b->pcm + have, 0, (need - have) * sizeof(float));
				LoadInstall li {};
				li.track = t; li.slot = s; li.buf = b; li.meta = meta;
				computeThumb(b->pcm, frames, li.thumb);
				if (!engine.submitLoad(li))
					recycle(b);
			}
		}
		if (!did)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	// Drain what's left so nothing leaks on shutdown; finish any pending saves first so a
	// just-captured take reaches disk even if the module is closing.
	while (engine.cmds.pop(c)) {
		if (c.kind == Cmd::RELEASE)
			recycle(c.a);
		else if (c.kind == Cmd::SAVE && engine.sink && c.a)
			engine.sink->save(c.track, c.slot, c.a->pcm, c.meta);
		else if (c.kind == Cmd::CLEAR_FILE && engine.sink)
			engine.sink->clear(c.track, c.slot);
		else if (c.kind == Cmd::EVENT && engine.sink)
			engine.sink->event(c.ev);
	}
	if (engine.sink)
		engine.sink->flush();
}

} // namespace looper
} // namespace akaudio
