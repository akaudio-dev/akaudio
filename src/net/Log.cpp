// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "Log.hpp"

#include <atomic>

namespace akaudio {

namespace {
std::atomic<void (*)(const std::string&)> g_sink{nullptr};
}

void netLogSetSink(void (*sink)(const std::string&)) {
	g_sink.store(sink, std::memory_order_release);
}

void netLog(const std::string& msg) {
	void (*sink)(const std::string&) = g_sink.load(std::memory_order_acquire);
	if (sink)
		sink(msg);
}

} // namespace akaudio
