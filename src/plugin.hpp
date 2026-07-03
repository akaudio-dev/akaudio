// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
#include <rack.hpp>

using namespace rack;

// Declared in plugin.cpp; the single Plugin instance for this collection.
extern Plugin* pluginInstance;

// One extern Model* per module. Add a line here for every new module.
extern Model* modelNinjam;
extern Model* modelRadio;

// Safe jansson string read for dataFromJson()/config files: a malformed patch
// storing a non-string (json_string_value returns NULL) must not crash via
// std::string(NULL). Returns `def` when absent or not a string.
inline std::string jsonStr(json_t* j, const std::string& def = "") {
	return (j && json_is_string(j)) ? json_string_value(j) : def;
}
