// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "Socket.hpp"
#include "Http.hpp"
#include "Log.hpp"
#include "Tls.hpp"

#include <cctype>
#include <cstring>

namespace akaudio {

Url parseUrl(const std::string& url) {
	Url u;
	// A valid URL carries no raw control bytes. Reject any (CR/LF and other C0, plus
	// DEL) so a bare CR or CRLF smuggled in via a playlist line or a redirect Location
	// can't splice extra headers into the "GET …\r\nHost: …" request built from u.
	// (A legitimately intended %0D stays literal '%','0','D' — those are fine.)
	for (unsigned char c : url)
		if (c < 0x20 || c == 0x7f)
			return u; // u.ok stays false
	std::string s;
	if (url.rfind("https://", 0) == 0) {
		u.tls = true;
		u.port = "443";
		s = url.substr(8);
	}
	else if (url.rfind("http://", 0) == 0) {
		s = url.substr(7);
	}
	else {
		return u; // unsupported scheme
	}

	size_t slash = s.find('/');
	std::string authority = (slash == std::string::npos) ? s : s.substr(0, slash);
	u.path = (slash == std::string::npos) ? "/" : s.substr(slash);

	size_t colon = authority.find(':');
	if (colon == std::string::npos) {
		u.host = authority;
	}
	else {
		u.host = authority.substr(0, colon);
		u.port = authority.substr(colon + 1);
	}
	u.ok = !u.host.empty();
	return u;
}

bool endsWithCI(const std::string& s, const std::string& suffix) {
	if (s.size() < suffix.size())
		return false;
	for (size_t i = 0; i < suffix.size(); i++) {
		char a = (char) std::tolower((unsigned char) s[s.size() - suffix.size() + i]);
		char b = (char) std::tolower((unsigned char) suffix[i]);
		if (a != b)
			return false;
	}
	return true;
}

std::string pathNoQuery(const std::string& url) {
	std::string p = parseUrl(url).path;
	size_t q = p.find('?');
	return q == std::string::npos ? p : p.substr(0, q);
}

std::string urlJoin(const std::string& base, const std::string& ref) {
	if (ref.rfind("http://", 0) == 0 || ref.rfind("https://", 0) == 0)
		return ref; // absolute
	if (ref.rfind("//", 0) == 0) { // scheme-relative
		size_t s = base.find("://");
		std::string scheme = (s == std::string::npos) ? "http:" : base.substr(0, s + 1);
		return scheme + ref;
	}
	if (!ref.empty() && ref[0] == '/') { // host-rooted
		size_t s = base.find("://");
		if (s != std::string::npos) {
			size_t h = base.find('/', s + 3);
			std::string origin = (h == std::string::npos) ? base : base.substr(0, h);
			return origin + ref;
		}
	}
	// Relative to the base's directory. The directory is taken from the PATH only:
	// strip the base's query/fragment first, else the last '/' could land inside a
	// "?tok=ab/cd" query (garbage segment URLs) or after the authority of a
	// query-only base. Per RFC 3986 a relative ref also drops the base's query.
	size_t s = base.find("://");
	size_t from = (s == std::string::npos) ? 0 : s + 3;
	size_t pathEnd = base.find_first_of("?#", from);
	size_t authEnd = base.find('/', from); // first '/' after the authority
	if (authEnd == std::string::npos || (pathEnd != std::string::npos && authEnd >= pathEnd))
		return base.substr(0, pathEnd) + "/" + ref; // no path segment → root the ref
	size_t slash = base.rfind('/', pathEnd == std::string::npos ? std::string::npos : pathEnd - 1);
	return base.substr(0, slash + 1) + ref;
}

long httpReadIdle(const Tls& tls, int fd, void* buf, size_t n,
		const std::atomic<bool>* abort, int sliceMs, int budgetMs) {
	int idleMs = 0;
	for (;;) {
		if (abort && abort->load(std::memory_order_acquire))
			return 0;
		long r = tlsRead(tls, fd, buf, n);
		if (r != -2)
			return r; // >0 data, 0 EOF, -1 error
		idleMs += sliceMs; // recv timed out with no data; retry within the budget
		if (idleMs >= budgetMs) {
			netLog("read idle for " + std::to_string(budgetMs) + " ms — treating as end of stream");
			return 0;
		}
	}
}

// Case-insensitive search for a header value (e.g. "location"). Returns "" if
// absent. `headers` is the block before the blank line.
std::string headerValue(const std::string& headers, const std::string& name) {
	std::string lower;
	lower.reserve(headers.size());
	for (char c : headers)
		lower += (char) std::tolower((unsigned char) c);
	std::string key = "\n" + name + ":";
	for (char& c : key)
		c = (char) std::tolower((unsigned char) c);
	size_t p = lower.find(key);
	if (p == std::string::npos)
		return "";
	p += key.size();
	size_t end = headers.find("\r\n", p);
	std::string v = headers.substr(p, end == std::string::npos ? std::string::npos : end - p);
	size_t a = v.find_first_not_of(" \t");
	size_t b = v.find_last_not_of(" \t\r\n");
	return (a == std::string::npos) ? "" : v.substr(a, b - a + 1);
}

namespace {

bool aborted(const std::atomic<bool>* abort) {
	return abort && abort->load(std::memory_order_acquire);
}

// httpGet's per-recv timeout / idle ceiling: short enough that an abort during a
// station switch is noticed quickly, long enough for slow directory APIs.
constexpr int kGetSliceMs = 700;
constexpr int kGetIdleBudgetMs = 8000;

void closeConn(HttpConn& c) {
	if (c.fd < 0)
		return;
	tlsFree(c.tls);
	netClose(c.fd);
	c.fd = -1;
}

} // namespace

bool httpOpen(const Url& u, const char* extraHeader, const std::atomic<bool>* abort,
		int connectTimeoutMs, int recvSliceMs, int idleBudgetMs,
		GuardedFd* sockOut, HttpConn& out, std::string* err, bool blockPrivate) {
	auto fail = [&](const std::string& e) {
		if (err)
			*err = e;
		return false;
	};

	std::string connErr;
	int fd = netResolveConnect(u.host, u.port, abort, connectTimeoutMs, &connErr, blockPrivate);
	if (fd < 0)
		return fail(connErr);
	// From here on the caller owns the socket (see the header contract) — publish
	// it first so a concurrent stop() can shutdown() any blocking call below.
	out.fd = fd;
	if (sockOut)
		sockOut->publish(fd);
	// Bound every recv (and send) so a silent peer can't hang us.
	netSetRcvTimeout(fd, recvSliceMs);
	netSetSndTimeout(fd, recvSliceMs);

	if (u.tls && !tlsHandshake(out.tls, fd, u.host)) // tlsHandshake logs the reason
		return fail("TLS handshake failed");

	std::string req =
		"GET " + u.path + " HTTP/1.0\r\n"
		"Host: " + u.host + "\r\n"
		"User-Agent: AKAudio-VCVRack/2.0\r\n"
		+ (extraHeader && *extraHeader ? std::string(extraHeader) + "\r\n" : std::string()) +
		"Connection: close\r\n"
		"\r\n";
	if (tlsWrite(out.tls, fd, req.data(), req.size()) < 0)
		return fail("Send failed");

	// Read headers up to the blank line; keep any body bytes already read.
	std::string head;
	char tmp[2048];
	size_t headerEnd = std::string::npos;
	while (!aborted(abort) && head.size() < (1 << 16)) {
		long n = httpReadIdle(out.tls, fd, tmp, sizeof(tmp), abort, recvSliceMs, idleBudgetMs);
		if (n <= 0)
			break;
		head.append(tmp, n);
		headerEnd = head.find("\r\n\r\n");
		if (headerEnd != std::string::npos)
			break;
	}
	if (headerEnd == std::string::npos) {
		netLog("no HTTP response from " + u.host + ":" + u.port + u.path);
		return fail("No HTTP response");
	}

	out.headers = head.substr(0, headerEnd);
	out.leftover = head.substr(headerEnd + 4);
	// Log only an ABNORMAL status line (4xx/5xx — e.g. a rotted station's 410
	// Gone). Healthy 2xx/3xx are the expected case and stay silent, so log.txt
	// volume scales with problems, not with traffic (HLS re-fetches segments
	// every few seconds for hours).
	std::string statusLine = out.headers.substr(0, out.headers.find("\r\n"));
	if (statusLine.find(" 20") == std::string::npos
			&& statusLine.find(" 30") == std::string::npos)
		netLog("HTTP " + u.host + u.path + " → " + statusLine);
	return true;
}

bool httpGet(const std::string& url, std::string& out, const std::atomic<bool>* abort,
		size_t maxBytes) {
	out.clear();
	std::string current = url;
	for (int hop = 0; hop < 5 && !aborted(abort); hop++) { // follow up to 5 redirects
		Url u = parseUrl(current);
		if (!u.ok)
			return false;

		HttpConn c;
		// hop 0 is the caller's own URL (may legitimately be LAN/localhost); redirect
		// targets (hop > 0) are blocked from private/internal addresses (SSRF guard).
		if (!httpOpen(u, "Accept: */*", abort, 6000, kGetSliceMs, kGetIdleBudgetMs, nullptr, c,
				nullptr, /*blockPrivate=*/hop > 0)) {
			closeConn(c);
			return false;
		}
		std::string statusLine = c.headers.substr(0, c.headers.find("\r\n"));

		// Redirects: 301/302/303/307/308 with a Location header.
		if (statusLine.find(" 30") != std::string::npos) {
			std::string loc = headerValue(c.headers, "location");
			bool wasTls = u.tls;
			closeConn(c);
			if (loc.empty())
				return false;
			std::string next = urlJoin(current, loc);
			// Refuse a https→http downgrade: an on-path attacker (or malicious
			// redirector) must not be able to strip TLS on a later hop.
			if (wasTls && parseUrl(next).tls == false) {
				netLog("redirect BLOCKED: https\xe2\x86\x92http downgrade");
				return false;
			}
			current = next;
			continue;
		}
		if (statusLine.find("200") == std::string::npos) {
			closeConn(c);
			return false;
		}

		// Drain the body (bounded — this is for kilobyte-sized replies).
		std::string body = std::move(c.leftover);
		bool tooBig = false;
		char buf[4096];
		for (;;) {
			long n = httpReadIdle(c.tls, c.fd, buf, sizeof(buf), abort, kGetSliceMs, kGetIdleBudgetMs);
			if (n <= 0)
				break; // EOF, error, abort, or idle budget exhausted
			body.append(buf, n);
			if (body.size() > maxBytes) {
				// Over the ceiling: this is not a "small body" — treat it as a
				// failure rather than silently handing back a truncated payload.
				tooBig = true;
				break;
			}
		}
		closeConn(c);
		if (tooBig || aborted(abort))
			return false;
		out = std::move(body);
		return true;
	}
	return false; // too many redirects
}

} // namespace akaudio
