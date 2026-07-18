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

std::string redactUrl(const std::string& url) {
	std::string s = url;
	// Strip userinfo in the authority: scheme://user:pass@host → scheme://host. The '@'
	// must precede the first '/' (i.e. sit in the authority) so a '@' in a path/query
	// isn't mistaken for userinfo.
	size_t scheme = s.find("://");
	size_t hostStart = (scheme == std::string::npos) ? 0 : scheme + 3;
	size_t at = s.find('@', hostStart);
	size_t slash = s.find('/', hostStart);
	if (at != std::string::npos && (slash == std::string::npos || at < slash))
		s = s.substr(0, hostStart) + s.substr(at + 1);
	// Drop any query string (session tokens live there); mark that one was present.
	size_t q = s.find('?');
	if (q != std::string::npos)
		s = s.substr(0, q) + "?\xe2\x80\xa6"; // "?…"
	return s;
}

} // namespace akaudio
