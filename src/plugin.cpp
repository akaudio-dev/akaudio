// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "plugin.hpp"

#include <csignal>
#include <string>

namespace akaudio {
void netStartup(); // net/Socket.cpp — one-time Winsock init (no-op on POSIX)
void netLogSetSink(void (*)(const std::string&)); // net/Log.cpp — diagnostics sink
}

Plugin* pluginInstance;

// Rack calls init() once when loading the shared library.
// Register every module's Model here.
void init(Plugin* p) {
	pluginInstance = p;

	// Initialize Winsock once before any networking (no-op on macOS/Linux).
	akaudio::netStartup();

	// Route net-layer lifecycle diagnostics (connects, TLS/HTTP failures, stream
	// endings — one-liners, never per-packet) into Rack's log.txt, so "why won't
	// this station play?" is answerable from a user's log instead of a debugger.
	akaudio::netLogSetSink([](const std::string& m) { INFO("akaudio.net: %s", m.c_str()); });

	// Safety net: never let a write to a closed/shutdown socket terminate the host
	// via SIGPIPE. Our sockets also set SO_NOSIGPIPE; this covers everything else
	// (e.g. Ninjam transmit) regardless of platform. Default SIGPIPE action kills
	// the process and leaves no crash report. (Windows has no SIGPIPE.)
#ifdef SIGPIPE
	std::signal(SIGPIPE, SIG_IGN);
#endif

	p->addModel(modelNinjam);
	p->addModel(modelRadio);
}
