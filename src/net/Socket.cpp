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

int netConnectAbortable(addrinfo* res, const std::atomic<bool>* abort, int timeoutMs) {
	auto aborted = [abort]() { return abort && abort->load(std::memory_order_acquire); };
	const int slices = timeoutMs > 0 ? (timeoutMs + 99) / 100 : 1;

	for (addrinfo* ai = res; ai; ai = ai->ai_next) {
		int fd = (int) ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
#ifdef SO_NOSIGPIPE
		// Make writes to a peer-closed socket return EPIPE instead of raising
		// SIGPIPE (which would kill the host). Not defined on Linux/Windows;
		// there plugin.cpp's SIG_IGN / Winsock semantics cover it.
		int nosig = 1;
		::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const char*) &nosig, sizeof(nosig));
#endif
		netSetNonBlocking(fd, true);
		int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
		bool ok = (rc == 0);
		if (rc < 0 && netConnectInProgress()) {
			for (int i = 0; i < slices && !aborted(); i++) {
				fd_set wf;
				FD_ZERO(&wf);
				FD_SET(fd, &wf);
				timeval tv{0, 100 * 1000}; // 100 ms
				int s = ::select(fd + 1, nullptr, &wf, nullptr, &tv);
				if (s > 0) {
					ok = (netSoError(fd) == 0);
					break;
				}
				if (s < 0 && !netInterrupted())
					break;
			}
		}
		if (ok) {
			netSetNonBlocking(fd, false); // back to blocking for the caller's read loop
			return fd;
		}
		netClose(fd);
		if (aborted())
			break;
	}
	return -1;
}

} // namespace akaudio
