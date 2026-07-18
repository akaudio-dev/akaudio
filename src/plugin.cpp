// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "plugin.hpp"

#include <string>

namespace akaudio {
void netLogSetSink(void (*)(const std::string&)); // net/Log.cpp — diagnostics sink
}

Plugin* pluginInstance;

// Rack calls init() once when loading the shared library.
// Register every module's Model here. No network setup happens here: Winsock
// init and the SIGPIPE guard run lazily inside netResolveConnect the first time
// a Module actually opens a connection (net/Socket.cpp netStartup()).
void init(Plugin* p) {
	pluginInstance = p;

	// Route net-layer lifecycle diagnostics (connects, TLS/HTTP failures, stream
	// endings — one-liners, never per-packet) into Rack's log.txt, so "why won't
	// this station play?" is answerable from a user's log instead of a debugger.
	// This only stores a function pointer — no network activity.
	akaudio::netLogSetSink([](const std::string& m) { INFO("akaudio.net: %s", m.c_str()); });

	p->addModel(modelNinjam);
	p->addModel(modelRadio);
}
