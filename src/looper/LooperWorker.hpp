// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
// LooperWorker — the Looper's background thread: the ONLY place buffers are
// allocated or freed (docs/LOOPER_DESIGN.md §5.4). Serves the engine's commands in
// order: ALLOC (a fresh N-frame buffer), OVERDUB_COPY (staging = take + the rolling
// buffer's completed part), RELEASE (recycle). Keeps a small free-list of
// same-size buffers so arming never waits on malloc; flushes it when N changes.
// It also runs the M4 disk jobs (SAVE / CLEAR_FILE) and the manifest flush by calling the
// engine's LooperSink — the encode + file I/O sit here, off the audio thread, where the
// take buffer's lifetime is guaranteed (its RELEASE is ordered after its SAVE).
#include <atomic>
#include <thread>
#include <vector>

#include "LooperEngine.hpp"

namespace akaudio {
namespace looper {

class LooperWorker {
public:
	explicit LooperWorker(LooperEngine& e) : engine(e) {}
	~LooperWorker();
	void start();
	void stop(); // sets quit, joins; frees the pool

private:
	void run();
	Buf* alloc(int frames);
	void recycle(Buf* b);

	LooperEngine& engine;
	std::thread thread;
	std::atomic<bool> quit{false};
	std::vector<Buf*> pool; // worker thread only
	static const size_t POOL_MAX = 4;
};

} // namespace looper
} // namespace akaudio
