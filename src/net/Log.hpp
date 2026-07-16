// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
#include <string>

// Pluggable diagnostics sink for the net/ layer. This code is Rack-free, so it
// cannot call Rack's logger directly; instead plugin init() routes netLog into
// Rack's INFO (→ log.txt) and the standalone test harnesses route it to stderr.
// Default sink: none (messages dropped).
//
// Discipline: log ONLY THE ABNORMAL — failures with reasons and timings
// (resolve/connect/TLS/HTTP), unexpected stream endings, idle timeouts. The
// expected lifecycle (connects, playing, user stops, healthy re-polls) stays
// silent: a quiet log.txt means a healthy plugin, and every line that does
// appear deserves attention. One-liners; never per-packet; never from the
// audio thread. The point is that a user's log.txt answers "why couldn't it
// connect?" without a debugger.

namespace akaudio {

void netLogSetSink(void (*sink)(const std::string&));
void netLog(const std::string& msg);

} // namespace akaudio
