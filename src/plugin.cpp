// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrey Kozlov

#include "plugin.hpp"

#include <csignal>

Plugin* pluginInstance;

// Rack calls init() once when loading the shared library.
// Register every module's Model here.
void init(Plugin* p) {
	pluginInstance = p;

	// Safety net: never let a write to a closed/shutdown socket terminate the host
	// via SIGPIPE. Our sockets also set SO_NOSIGPIPE; this covers everything else
	// (e.g. Ninjam transmit) regardless of platform. Default SIGPIPE action kills
	// the process and leaves no crash report.
	std::signal(SIGPIPE, SIG_IGN);

	p->addModel(modelNinjam);
	p->addModel(modelRadio);
}
