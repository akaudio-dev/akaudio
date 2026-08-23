// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "LooperWorker.hpp"

#include <chrono>
#include <cstring>

namespace akaudio {
namespace looper {

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
					Reply r;
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
					Reply r;
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
			}
		}
		if (engine.sink)
			engine.sink->flush(); // write the manifest if UI edits (names / settings) dirtied it
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
	}
	if (engine.sink)
		engine.sink->flush();
}

} // namespace looper
} // namespace akaudio
