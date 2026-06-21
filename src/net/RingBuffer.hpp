#pragma once
#include <atomic>
#include <vector>
#include <cstddef>

namespace akaudio {

// Lock-free single-producer / single-consumer ring buffer of stereo float frames.
//
// One thread (the network/decode thread) calls push(); one thread (the audio
// thread, inside Module::process()) calls pull(). No locks, no allocation on
// either hot path, so it is safe to read from in realtime.
//
// Capacity is rounded up to a power of two so index wrap is a cheap mask.
class StereoRingBuffer {
public:
	explicit StereoRingBuffer(size_t minCapacity = 1 << 16) {
		size_t cap = 1;
		while (cap < minCapacity)
			cap <<= 1;
		mask = cap - 1;
		buf.resize(cap);
	}

	// Producer side. Returns false if the buffer is full (caller should back off).
	bool push(float l, float r) {
		const size_t w = writeIdx.load(std::memory_order_relaxed);
		const size_t next = (w + 1) & mask;
		if (next == readIdx.load(std::memory_order_acquire))
			return false; // full
		buf[w].l = l;
		buf[w].r = r;
		writeIdx.store(next, std::memory_order_release);
		return true;
	}

	// Consumer side. Returns false (and leaves l/r untouched) on underrun.
	bool pull(float& l, float& r) {
		const size_t rd = readIdx.load(std::memory_order_relaxed);
		if (rd == writeIdx.load(std::memory_order_acquire))
			return false; // empty
		l = buf[rd].l;
		r = buf[rd].r;
		readIdx.store((rd + 1) & mask, std::memory_order_release);
		return true;
	}

	// Approximate number of frames available to the consumer.
	size_t size() const {
		const size_t w = writeIdx.load(std::memory_order_acquire);
		const size_t rd = readIdx.load(std::memory_order_acquire);
		return (w - rd) & mask;
	}

	size_t capacity() const { return mask; }

	// Consumer-side reset (only safe when the producer is stopped).
	void clear() {
		readIdx.store(0, std::memory_order_relaxed);
		writeIdx.store(0, std::memory_order_relaxed);
	}

private:
	struct Frame { float l = 0.f, r = 0.f; };
	std::vector<Frame> buf;
	size_t mask = 0;
	std::atomic<size_t> writeIdx{0};
	std::atomic<size_t> readIdx{0};
};

} // namespace akaudio
