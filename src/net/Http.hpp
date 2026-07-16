// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
#include <string>
#include <atomic>

#include "Tls.hpp"

namespace akaudio { class GuardedFd; } // Socket.hpp; only a pointer is needed here

// Tiny HTTP(S)/1.0 GET for small text/binary bodies (room/station directory JSON,
// playlists, favicons). Reads the whole response into a string — meant for
// kilobyte-sized replies, NOT for audio streams (those go through StreamClient).
// MUST be called off the UI/audio thread.
//
// Bounded + interruptible: a non-blocking connect and a per-recv timeout mean no
// call can block forever on a dead/silent peer, and if `abort` is supplied it
// bails promptly when set — so a caller that join()s this thread (e.g.
// StreamClient on a station switch) never freezes the UI. Follows redirects.

namespace akaudio {

// GET the url; on success returns true and fills out with the response body. On
// any failure returns false (out cleared). If abort is non-null and becomes true,
// the request is abandoned and false returned. A body larger than maxBytes is a
// failure, not a truncation — callers fetching something known-small (a playlist)
// can pass a tight cap to bail out fast when the URL turns out to be an endless
// audio stream. Failures netLog their reason; success is silent.
bool httpGet(const std::string& url, std::string& out, const std::atomic<bool>* abort = nullptr,
             size_t maxBytes = 4 << 20);

// Parsed http(s)://host[:port][/path]. Shared by Http and StreamClient (one
// parser, one set of quirks).
struct Url {
	std::string host;
	std::string port = "80";
	std::string path = "/";
	bool tls = false;
	bool ok = false;
};
Url parseUrl(const std::string& url);

// Case-insensitive suffix check (used for playlist/HLS extension sniffing).
bool endsWithCI(const std::string& s, const std::string& suffix);

// Path part of a URL, without the query string (for extension checks).
std::string pathNoQuery(const std::string& url);

// Resolve a possibly-relative reference (redirect Location, playlist/segment URI)
// against a base URL: absolute, scheme-relative (//…), host-rooted (/…), or
// relative to the base's directory.
std::string urlJoin(const std::string& base, const std::string& ref);

// Case-insensitive lookup of a response-header value (e.g. "location") in a
// header block; "" if absent.
std::string headerValue(const std::string& headers, const std::string& name);

// Bounded, abortable read over a (possibly TLS) socket whose SO_RCVTIMEO is set
// to sliceMs: each timed-out slice polls `abort` and accumulates toward budgetMs.
// Returns >0 data, 0 on EOF / abort / idle-budget-exhausted, -1 on a real error.
long httpReadIdle(const Tls& tls, int fd, void* buf, size_t n,
                  const std::atomic<bool>* abort, int sliceMs, int budgetMs);

// A connected, header-parsed HTTP(S) exchange, shared by httpGet (small bodies)
// and StreamClient (endless audio bodies).
struct HttpConn {
	int fd = -1;          // connected socket; <0 = never connected
	Tls tls;              // active for https (tlsRead/tlsWrite no-op through it for http)
	std::string headers;  // status line + headers (block before the blank line)
	std::string leftover; // body bytes that arrived with the headers
};

// Resolve+connect (abortable), TLS handshake for https, send "GET path HTTP/1.0"
// (Host/User-Agent/Connection: close + the optional extraHeader line), and read
// the response headers. recvSliceMs also becomes the socket's recv/send timeout.
// If sockOut is non-null the fd is published into it the moment it connects, so
// another thread can sockOut->shutdown() it to interrupt any later blocking call.
// OWNERSHIP: once out.fd >= 0 the CALLER owns the socket + TLS state and must
// clean up (tlsFree + netClose, or sockOut->closeOwned()) on both success and
// failure — httpOpen never closes a connected fd (a publisher's stop() may be
// racing us on it). Returns true when the headers were read; false with a reason
// in *err.
bool httpOpen(const Url& u, const char* extraHeader, const std::atomic<bool>* abort,
              int connectTimeoutMs, int recvSliceMs, int idleBudgetMs,
              GuardedFd* sockOut, HttpConn& out, std::string* err = nullptr);

} // namespace akaudio
