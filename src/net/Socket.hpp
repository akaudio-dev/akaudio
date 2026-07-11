// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov
//
// Cross-platform socket compatibility shim (POSIX BSD sockets <-> Winsock2).
// The net/ layer is written against POSIX sockets; on Windows the same calls map
// to Winsock2 with small surface differences (handle type, error reporting,
// non-blocking control, timeout option encoding, and char* buffer params). This
// header hides those behind a tiny portable surface so the .cpp files stay clean.
//
// Sockets are stored as `int` throughout the codebase (e.g. std::atomic<int> sock).
// On Win64 a SOCKET is an unsigned pointer-sized handle, but the kernel guarantees
// socket handles fit well within 32 bits, and INVALID_SOCKET ((SOCKET)~0) maps to
// -1 as an int — so the existing `fd < 0` / `-1` conventions keep working.
//
// Include this header BEFORE anything that may pull in <windows.h> (winsock2.h must
// precede windows.h). The net .cpp files are Rack-free and include it first.
#pragma once

#include <atomic>
#include <string>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#endif

namespace akaudio {

// One-time Winsock initialization (no-op on POSIX). Idempotent and thread-safe
// enough for our use (called from init() before any socket work). Defined in
// Socket.cpp so callers that can't include <winsock2.h> (plugin.cpp pulls in Rack
// headers / <windows.h>) can just forward-declare and call it.
void netStartup();

// Close a socket fd.
inline int netClose(int fd) {
#ifdef _WIN32
	return ::closesocket((SOCKET) fd);
#else
	return ::close(fd);
#endif
}

// Shut a socket down both ways (used to interrupt a blocked recv from another
// thread; run()/the owning thread still performs the close).
inline int netShutdown(int fd) {
#ifdef _WIN32
	return ::shutdown((SOCKET) fd, SD_BOTH);
#else
	return ::shutdown(fd, SHUT_RDWR);
#endif
}

// Put a socket into (non-)blocking mode. Returns true on success.
inline bool netSetNonBlocking(int fd, bool nonblock) {
#ifdef _WIN32
	u_long m = nonblock ? 1 : 0;
	return ::ioctlsocket((SOCKET) fd, FIONBIO, &m) == 0;
#else
	int flags = ::fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return false;
	if (nonblock)
		flags |= O_NONBLOCK;
	else
		flags &= ~O_NONBLOCK;
	return ::fcntl(fd, F_SETFL, flags) == 0;
#endif
}

// recv/send timeout in milliseconds. POSIX encodes it as a timeval; Windows as a
// DWORD of milliseconds — hence the shim instead of a raw setsockopt at call sites.
inline void netSetRcvTimeout(int fd, int ms) {
#ifdef _WIN32
	DWORD t = (DWORD) ms;
	::setsockopt((SOCKET) fd, SOL_SOCKET, SO_RCVTIMEO, (const char*) &t, sizeof(t));
#else
	timeval tv{ms / 1000, (ms % 1000) * 1000};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

inline void netSetSndTimeout(int fd, int ms) {
#ifdef _WIN32
	DWORD t = (DWORD) ms;
	::setsockopt((SOCKET) fd, SOL_SOCKET, SO_SNDTIMEO, (const char*) &t, sizeof(t));
#else
	timeval tv{ms / 1000, (ms % 1000) * 1000};
	::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

// Enable TCP_NODELAY (Nagle off) for low-latency small protocol messages.
inline void netSetTcpNoDelay(int fd) {
	int one = 1;
#ifdef _WIN32
	::setsockopt((SOCKET) fd, IPPROTO_TCP, TCP_NODELAY, (const char*) &one, sizeof(one));
#else
	::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
}

// Read the SO_ERROR pending error after a non-blocking connect completes.
inline int netSoError(int fd) {
	int err = 0;
	socklen_t len = sizeof(err);
#ifdef _WIN32
	::getsockopt((SOCKET) fd, SOL_SOCKET, SO_ERROR, (char*) &err, &len);
#else
	::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
#endif
	return err;
}

// Last socket error, as portable predicates over errno / WSAGetLastError().
inline bool netWouldBlock() {
#ifdef _WIN32
	int e = WSAGetLastError();
	return e == WSAEWOULDBLOCK || e == WSAETIMEDOUT;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

// A non-blocking connect() that hasn't finished yet.
inline bool netConnectInProgress() {
#ifdef _WIN32
	return WSAGetLastError() == WSAEWOULDBLOCK;
#else
	return errno == EINPROGRESS;
#endif
}

// recv/send/select interrupted by a signal (POSIX EINTR; never happens on Windows).
inline bool netInterrupted() {
#ifdef _WIN32
	return false;
#else
	return errno == EINTR;
#endif
}

// Non-blocking connect across a getaddrinfo result that polls `abort` every
// 100 ms, up to ~timeoutMs per address. Returns the connected fd (restored to
// blocking mode) or -1. This is THE way to connect in this plugin: a dead/slow
// host can never wedge a stop()/join() on the UI thread, because the loop
// notices `abort` within 100 ms. Sets SO_NOSIGPIPE where available. Defined in
// Socket.cpp; shared by Http, StreamClient, and NjClient.
int netConnectAbortable(addrinfo* res, const std::atomic<bool>* abort, int timeoutMs = 6000);

// Resolve host:port and connect abortably (getaddrinfo + netConnectAbortable).
// Returns the connected fd, or -1 with a human-readable reason in *errOut
// ("Cannot resolve host" / "Connection failed"; check `abort` yourself to tell
// an abort apart). Defined in Socket.cpp; the single resolve+connect preamble
// shared by Http, StreamClient, and NjClient.
int netResolveConnect(const std::string& host, const std::string& port,
                      const std::atomic<bool>* abort, int timeoutMs,
                      std::string* errOut = nullptr);

// Human-readable last socket error (for logs).
inline std::string netErrorStr() {
#ifdef _WIN32
	return "socket error " + std::to_string(WSAGetLastError());
#else
	return std::strerror(errno);
#endif
}

} // namespace akaudio
