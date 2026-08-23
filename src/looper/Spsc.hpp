// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
// Single-producer / single-consumer fixed-capacity queue of PODs (lock-free; the
// looper's audio<->worker and UI->audio channels). push() returns false when full —
// callers size CAP so that never happens in practice and treat it as "dropped".
#include <atomic>
#include <cstddef>

namespace akaudio {
namespace looper {

template <typename T, size_t CAP> // CAP must be a power of two
class SpscQueue {
public:
	bool push(const T& v) {
		size_t h = head.load(std::memory_order_relaxed);
		size_t n = (h + 1) & (CAP - 1);
		if (n == tail.load(std::memory_order_acquire))
			return false;
		buf[h] = v;
		head.store(n, std::memory_order_release);
		return true;
	}
	bool pop(T& out) {
		size_t t = tail.load(std::memory_order_relaxed);
		if (t == head.load(std::memory_order_acquire))
			return false;
		out = buf[t];
		tail.store((t + 1) & (CAP - 1), std::memory_order_release);
		return true;
	}
	bool empty() const { return tail.load(std::memory_order_acquire) == head.load(std::memory_order_acquire); }

private:
	T buf[CAP];
	std::atomic<size_t> head{0}, tail{0};
};

} // namespace looper
} // namespace akaudio
