// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

namespace akaudio {

// Lock-free single-producer / single-consumer ring buffer of fixed-width audio
// frames (FrameFloats floats per frame: 2 = stereo, RING_CH = the NINJAM wide mix).
//
// One thread (the network/decode thread) calls push(); one thread (the audio
// thread, inside Module::process()) calls pull(). No locks, no allocation on
// either hot path, so it is safe to read from in realtime.
//
// Capacity is rounded up to a power of two so index wrap is a cheap mask.
// (Usable capacity is one frame less than the rounded size — one slot is
// sacrificed to distinguish full from empty.)
//
// Resetting: there is deliberately NO thread-agnostic clear(). Indices may only
// be written by their owning side, so a third thread (the UI) resetting both
// would race a concurrent pull()/push() and corrupt the read/write relationship
// (the old failure mode: a station switch could replay a full ring of stale
// audio). Instead:
//   - requestClear() (any thread) flags the buffer; the CONSUMER applies the
//     drain on its next pull(), which returns false for that call.
//   - drain() discards everything buffered; call it from the consumer thread only.
template <int FrameFloats>
class FrameRingBuffer {
public:
	explicit FrameRingBuffer(size_t minFrames = 1 << 16) {
		size_t cap = 1;
		while (cap < minFrames)
			cap <<= 1;
		mask = cap - 1;
		buf.resize(cap * FrameFloats);
	}

	// Producer side. `frame` = FrameFloats floats. Returns false if the buffer is
	// full (caller should back off).
	bool push(const float* frame) {
		const size_t w = writeIdx.load(std::memory_order_relaxed);
		const size_t next = (w + 1) & mask;
		if (next == readIdx.load(std::memory_order_acquire))
			return false; // full
		std::memcpy(&buf[w * FrameFloats], frame, FrameFloats * sizeof(float));
		writeIdx.store(next, std::memory_order_release);
		return true;
	}

	// Consumer side. Returns false (and leaves `frame` untouched) on underrun,
	// and false once when applying a pending requestClear().
	bool pull(float* frame) {
		if (clearReq.load(std::memory_order_relaxed)) {
			clearReq.store(false, std::memory_order_relaxed);
			drain();
			return false;
		}
		const size_t rd = readIdx.load(std::memory_order_relaxed);
		if (rd == writeIdx.load(std::memory_order_acquire))
			return false; // empty
		std::memcpy(frame, &buf[rd * FrameFloats], FrameFloats * sizeof(float));
		readIdx.store((rd + 1) & mask, std::memory_order_release);
		return true;
	}

	// Approximate number of frames available to the consumer.
	size_t size() const {
		const size_t w = writeIdx.load(std::memory_order_acquire);
		const size_t rd = readIdx.load(std::memory_order_acquire);
		return (w - rd) & mask;
	}

	// Usable frame capacity (rounded size - 1; see class comment).
	size_t capacity() const { return mask; }

	// Ask the consumer to discard everything buffered (safe from any thread; the
	// drop happens inside the consumer's next pull()). Used on stream start/stop
	// so a new station never replays the previous one's tail.
	void requestClear() { clearReq.store(true, std::memory_order_relaxed); }

	// Discard everything currently buffered. CONSUMER THREAD ONLY: readIdx must
	// not be written by anyone else. Racing a concurrent push() is fine (it only
	// frees more space).
	void drain() {
		readIdx.store(writeIdx.load(std::memory_order_acquire), std::memory_order_release);
	}

private:
	std::vector<float> buf;
	size_t mask = 0;
	std::atomic<size_t> writeIdx{0};
	std::atomic<size_t> readIdx{0};
	std::atomic<bool> clearReq{false};
};

// Stereo convenience wrapper (the common case: one decoded L/R stream).
class StereoRingBuffer : public FrameRingBuffer<2> {
public:
	using FrameRingBuffer<2>::FrameRingBuffer;
	using FrameRingBuffer<2>::push;
	using FrameRingBuffer<2>::pull;

	bool push(float l, float r) {
		float f[2] = {l, r};
		return FrameRingBuffer<2>::push(f);
	}
	bool pull(float& l, float& r) {
		float f[2];
		if (!FrameRingBuffer<2>::pull(f))
			return false;
		l = f[0];
		r = f[1];
		return true;
	}
};

} // namespace akaudio
