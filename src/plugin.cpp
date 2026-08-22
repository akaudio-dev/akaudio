// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "plugin.hpp"

#include <string>

// Fonts defined in Theme.hpp
const char* FONT_BOLD = "res/fonts/Nunito-Bold.ttf";
const char* FONT_REG = "res/fonts/DejaVuSans.ttf";
const char* FONT_MONO = "res/fonts/ShareTechMono-Regular.ttf";

namespace akaudio {
void netLogSetSink(void (*)(const std::string&)); // net/Log.cpp — diagnostics sink
}

Plugin* pluginInstance;

// Rack calls init() once when loading the shared library.
// Register every module's Model here. No network setup happens here: Winsock
// init and the SIGPIPE guard run lazily inside netResolveConnect the first time
// a Module actually opens a connection (net/Socket.cpp netStartup()).
//
// `init` has C linkage from Rack's headers; Rack loads us by GetProcAddress("init").
// On Windows we mark it dllexport so it becomes the *only* exported symbol: MinGW
// otherwise auto-exports every global (works, but pointlessly exports FAAD2 /
// libvorbis / our net/ internals), and one explicit export flips the linker into
// export-only-what's-marked mode — with just init, the minimal surface Rack needs.
// (This is also why vendored FAAD2's own dllexport had to be blanked; see
// src/dep/faad2/include/neaacdec.h — otherwise it, not init, would be that surface.)
#ifdef _WIN32
__declspec(dllexport)
#endif
void init(Plugin* p) {
	pluginInstance = p;

	// Route net-layer lifecycle diagnostics (connects, TLS/HTTP failures, stream
	// endings — one-liners, never per-packet) into Rack's log.txt, so "why won't
	// this station play?" is answerable from a user's log instead of a debugger.
	// This only stores a function pointer — no network activity.
	akaudio::netLogSetSink([](const std::string& m) { INFO("akaudio.net: %s", m.c_str()); });

	p->addModel(modelLooper);
	p->addModel(modelNinjam);
	p->addModel(modelRadio);
}
