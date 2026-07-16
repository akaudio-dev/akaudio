// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "Socket.hpp"
#include "Log.hpp"

#include <chrono>
#include <memory>
#include <thread>

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

namespace {

// One in-flight getaddrinfo. There is no portable way to cancel getaddrinfo, so
// it runs on its own detached thread and the caller waits abortably; on timeout
// or abort the job is ABANDONED — the resolver thread finishes on its own time,
// sees `abandoned`, and frees the result itself. The thread touches nothing but
// this shared job (it holds the shared_ptr), so an abandoned resolve can never
// crash the caller. (Safe at process exit too: Rack never unloads the plugin
// dylib mid-run, and on quit the process — and this thread — just terminate.)
// Before this, a dead DNS (network down) parked every connecting bg thread in
// getaddrinfo for the resolver's own timeout (~30 s), and quitting Rack joined
// those threads one by one: the "Rack hangs on exit" report.
struct ResolveJob {
	std::string host, port;
	std::mutex mu;
	bool done = false;
	bool abandoned = false;
	addrinfo* res = nullptr;
};

void resolveThread(std::shared_ptr<ResolveJob> job) {
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* r = nullptr;
	if (::getaddrinfo(job->host.c_str(), job->port.c_str(), &hints, &r) != 0)
		r = nullptr;
	std::lock_guard<std::mutex> lock(job->mu);
	if (job->abandoned) {
		if (r)
			::freeaddrinfo(r); // nobody is waiting; clean up ourselves
	} else {
		job->res = r;
		job->done = true;
	}
}

// Resolve host:port with an abort-pollable bound. Returns the addrinfo list
// (caller frees) or nullptr.
addrinfo* resolveAbortable(const std::string& host, const std::string& port,
		const std::atomic<bool>* abort, int timeoutMs) {
	auto job = std::make_shared<ResolveJob>();
	job->host = host;
	job->port = port;
	std::thread(resolveThread, job).detach();

	for (int waited = 0;; waited += 25) {
		{
			std::lock_guard<std::mutex> lock(job->mu);
			if (job->done) {
				addrinfo* r = job->res;
				job->res = nullptr;
				return r;
			}
			if ((abort && abort->load(std::memory_order_acquire)) || waited >= timeoutMs) {
				job->abandoned = true; // resolver thread frees the result
				return nullptr;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}
}

} // namespace

int netResolveConnect(const std::string& host, const std::string& port,
		const std::atomic<bool>* abort, int timeoutMs, std::string* errOut) {
	using clock = std::chrono::steady_clock;
	auto ms = [](clock::time_point a, clock::time_point b) {
		return (int) std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
	};

	const clock::time_point t0 = clock::now();
	addrinfo* res = resolveAbortable(host, port, abort, timeoutMs);
	const clock::time_point t1 = clock::now();
	if (!res) {
		bool wasAborted = abort && abort->load(std::memory_order_acquire);
		netLog("resolve " + host + " FAILED after " + std::to_string(ms(t0, t1)) + " ms"
			+ (wasAborted ? " (aborted)" : " (DNS timeout/failure)"));
		if (errOut)
			*errOut = "Cannot resolve host";
		return -1;
	}

	int fd = netConnectAbortable(res, abort, timeoutMs);
	::freeaddrinfo(res);
	const clock::time_point t2 = clock::now();
	if (fd < 0) {
		bool wasAborted = abort && abort->load(std::memory_order_acquire);
		netLog("connect " + host + ":" + port + " FAILED after " + std::to_string(ms(t1, t2))
			+ " ms (resolve " + std::to_string(ms(t0, t1)) + " ms)"
			+ (wasAborted ? " (aborted)" : ": " + netErrorStr()));
		if (errOut)
			*errOut = "Connection failed";
		return -1;
	}
	// Success is silent: we log only the abnormal, so a quiet log.txt means a
	// healthy plugin. The failure lines above carry the timings for triage.
	return fd;
}

} // namespace akaudio
