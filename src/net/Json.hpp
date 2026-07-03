// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
#include <cstdlib>
#include <string>

#include <jansson.h>

// Tiny tolerant jansson accessors shared by the net/ JSON consumers
// (RoomDirectory, StationImport). Rack-free.

namespace akaudio {

// Accept a JSON integer, real, or numeric string (ninbot mixes all three).
inline int flexInt(json_t* v, int fallback = 0) {
	if (!v)
		return fallback;
	if (json_is_integer(v))
		return (int) json_integer_value(v);
	if (json_is_real(v))
		return (int) json_real_value(v);
	if (json_is_string(v)) {
		const char* s = json_string_value(v);
		if (s && *s)
			return std::atoi(s);
	}
	return fallback;
}

// String value, or "" when absent / not a string (never passes NULL to
// std::string).
inline std::string flexStr(json_t* v) {
	if (v && json_is_string(v))
		return json_string_value(v);
	return "";
}

} // namespace akaudio
