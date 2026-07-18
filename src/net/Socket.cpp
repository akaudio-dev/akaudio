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
	// Staggered PARALLEL connect ("happy eyeballs", RFC 8305 in spirit): launch a
	// new candidate every kStaggerMs while earlier attempts stay in flight, and
	// take the first that completes. Serial per-address attempts are hopeless
	// against hosts like RFI's live02 reflector: its DNS pool had 5 of 12 members
	// black-holed AND the resolver returns the dead ones first in a stable order,
	// so any serial budget split still burned the whole timeout without reaching a
	// live address. Racing them caps the damage of any number of dead members at
	// (dead-members x kStaggerMs) while a single live one answers within its RTT.
	auto aborted = [abort]() { return abort && abort->load(std::memory_order_acquire); };
	using clock = std::chrono::steady_clock;
	const clock::time_point deadline = clock::now()
		+ std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 1);
	constexpr int kStaggerMs = 300; // RFC 8305 suggests 100-2000 ms; SYN RTTs are ~10-300 ms

	int pending[FD_SETSIZE];
	int pendingIdx[FD_SETSIZE];
	int npending = 0;
	int addrCount = 0;
	for (addrinfo* ai = res; ai; ai = ai->ai_next)
		addrCount++;

	auto logCandidateFailed = [&](int idx, const addrinfo* ai, const char* what) {
		// A failed candidate while others remain is exactly the case that used to
		// be invisible (a dead member of a DNS round-robin set): one line so the
		// failover shows up in triage. Terminal failure is logged by
		// netResolveConnect with the host name and full timings.
		char addr[NI_MAXHOST] = "?";
		::getnameinfo(ai->ai_addr, ai->ai_addrlen, addr, sizeof(addr), nullptr, 0,
			NI_NUMERICHOST);
		netLog("connect: address " + std::to_string(idx + 1) + "/"
			+ std::to_string(addrCount) + " (" + addr + ") " + what);
	};
	// Map a pending slot back to its addrinfo (small lists; linear walk is fine).
	auto addrAt = [&](int idx) {
		addrinfo* ai = res;
		for (int i = 0; i < idx && ai; i++)
			ai = ai->ai_next;
		return ai;
	};
	auto closeAllPending = [&]() {
		for (int i = 0; i < npending; i++)
			netClose(pending[i]);
		npending = 0;
	};

	addrinfo* next = res;
	int nextIdx = 0;
	clock::time_point nextLaunch = clock::now();

	while (!aborted() && clock::now() < deadline) {
		// Launch the next candidate when its stagger slot arrives (immediately if
		// nothing is in flight — no reason to sit idle).
		while (next && npending < FD_SETSIZE
				&& (npending == 0 || clock::now() >= nextLaunch)) {
			addrinfo* ai = next;
			int idx = nextIdx;
			next = next->ai_next;
			nextIdx++;

			int fd = (int) ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
			if (fd < 0)
				continue; // e.g. no IPv6 support for an AAAA candidate; try the next
#ifndef _WIN32
			// POSIX fd_set is a bitmap indexed by the fd VALUE, so FD_SET on an fd
			// >= FD_SETSIZE is a stack out-of-bounds write. Only reachable with ~1024
			// descriptors already open; skip the candidate rather than corrupt memory.
			// (Windows fd_set is a count-bounded array — npending < FD_SETSIZE covers it.)
			if (fd >= FD_SETSIZE) {
				netLog("connect: socket fd " + std::to_string(fd)
					+ " >= FD_SETSIZE, skipping candidate (too many open descriptors)");
				netClose(fd);
				continue;
			}
#endif
#ifdef SO_NOSIGPIPE
			// Make writes to a peer-closed socket return EPIPE instead of raising
			// SIGPIPE (which would kill the host). Not defined on Linux/Windows;
			// there plugin.cpp's SIG_IGN / Winsock semantics cover it.
			int nosig = 1;
			::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const char*) &nosig, sizeof(nosig));
#endif
			netSetNonBlocking(fd, true);
			int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
			if (rc == 0) { // immediate success (loopback / same-host)
				closeAllPending();
				netSetNonBlocking(fd, false);
				return fd;
			}
			if (!netConnectInProgress()) {
				netClose(fd); // synchronous refusal; move on within this slot
				if (next || npending)
					logCandidateFailed(idx, ai, "refused, trying next");
				continue;
			}
			pending[npending] = fd;
			pendingIdx[npending] = idx;
			npending++;
			nextLaunch = clock::now() + std::chrono::milliseconds(kStaggerMs);
			break; // one launch per slot; back to polling
		}

		if (npending == 0 && !next)
			return -1; // every candidate failed synchronously

		// Poll all in-flight attempts for up to 100 ms (also the abort/stagger tick).
		fd_set wf;
		FD_ZERO(&wf);
		int maxFd = -1;
		for (int i = 0; i < npending; i++) {
			FD_SET(pending[i], &wf);
			if (pending[i] > maxFd)
				maxFd = pending[i];
		}
		timeval tv{0, 100 * 1000};
		int s = ::select(maxFd + 1, nullptr, npending ? &wf : nullptr, nullptr, &tv);
		if (s < 0 && !netInterrupted() && npending) {
			// select itself failed (shouldn't happen): fall back to failing out.
			break;
		}
		if (s > 0) {
			for (int i = 0; i < npending; i++) {
				if (!FD_ISSET(pending[i], &wf))
					continue;
				int fd = pending[i];
				const int winIdx = pendingIdx[i];
				if (netSoError(fd) == 0) {
					// Winner: first candidate to complete the handshake.
					pending[i] = pending[npending - 1];
					pendingIdx[i] = pendingIdx[npending - 1];
					npending--;
					closeAllPending();
					netSetNonBlocking(fd, false); // back to blocking for the caller's read loop
					if (winIdx > 0) {
						// Not the resolver's first choice — say so (one line per
						// connect, only when a failover actually happened).
						addrinfo* ai = addrAt(winIdx);
						if (ai)
							logCandidateFailed(winIdx, ai,
								"won the race (earlier candidates unresponsive)");
					}
					return fd;
				}
				// This candidate failed (RST etc.); drop it and keep racing.
				addrinfo* ai = addrAt(pendingIdx[i]);
				if (ai && (next || npending > 1))
					logCandidateFailed(pendingIdx[i], ai, "failed, still racing others");
				netClose(fd);
				pending[i] = pending[npending - 1];
				pendingIdx[i] = pendingIdx[npending - 1];
				npending--;
				i--;
			}
			if (npending == 0 && !next)
				return -1; // the last racer just failed
		}
	}

	// Deadline or abort: nobody completed.
	closeAllPending();
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
