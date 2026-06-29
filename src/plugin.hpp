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
