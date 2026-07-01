// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "Socket.hpp"

namespace akaudio {

void netStartup() {
#ifdef _WIN32
	static bool done = false;
	if (!done) {
		WSADATA wsa;
		WSAStartup(MAKEWORD(2, 2), &wsa);
		done = true; // never WSACleanup(): Winsock is needed for the whole process life
	}
#endif
}

} // namespace akaudio
