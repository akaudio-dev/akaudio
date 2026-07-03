// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "Http.hpp"
#include "Tls.hpp"
#include "Socket.hpp"

#include <cctype>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>

namespace akaudio {

Url parseUrl(const std::string& url) {
	Url u;
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

namespace {

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

// Resolve a redirect Location against the request URL (absolute, scheme-relative,
// or absolute-path forms — enough for the redirects favicons/APIs actually use).
std::string resolveRedirect(const Url& base, const std::string& loc) {
	if (loc.rfind("http://", 0) == 0 || loc.rfind("https://", 0) == 0)
		return loc;
	std::string scheme = base.tls ? "https://" : "http://";
	std::string authority = base.host + (base.port == "80" || base.port == "443" ? "" : ":" + base.port);
	if (loc.rfind("//", 0) == 0)
		return (base.tls ? "https:" : "http:") + loc;
	if (!loc.empty() && loc[0] == '/')
		return scheme + authority + loc;
	return scheme + authority + "/" + loc;
}

bool aborted(const std::atomic<bool>* abort) {
	return abort && abort->load(std::memory_order_acquire);
}

void setRcvTimeout(int fd, int ms) {
	netSetRcvTimeout(fd, ms);
	netSetSndTimeout(fd, ms);
}

// One request; returns the raw response (headers + body) or "" on error/abort.
std::string fetchOnce(const Url& u, const std::atomic<bool>* abort) {
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* res = nullptr;
	if (::getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &res) != 0 || !res)
		return "";
	int fd = netConnectAbortable(res, abort, 6000);
	::freeaddrinfo(res);
	if (fd < 0)
		return "";

	// Bound every recv so a silent peer can't hang us; short enough that an abort
	// during a station switch is noticed quickly.
	setRcvTimeout(fd, 700);

	Tls tls;
	if (u.tls && !tlsHandshake(tls, fd, u.host)) {
		netClose(fd);
		return "";
	}

	std::string req =
		"GET " + u.path + " HTTP/1.0\r\n"
		"Host: " + u.host + "\r\n"
		"User-Agent: AKAudio-VCVRack/2.0\r\n"
		"Accept: */*\r\n"
		"Connection: close\r\n"
		"\r\n";
	if (tlsWrite(tls, fd, req.data(), req.size()) < 0) {
		tlsFree(tls);
		netClose(fd);
		return "";
	}

	std::string resp;
	char buf[4096];
	const size_t maxResp = 4 << 20;            // 4 MiB ceiling
	const int maxIdleMs = 8000;                // give up if no data for this long
	int idleMs = 0;
	for (;;) {
		if (aborted(abort)) {
			resp.clear();
			break;
		}
		long n = tlsRead(tls, fd, buf, sizeof(buf));
		if (n > 0) {
			resp.append(buf, n);
			idleMs = 0;
			if (resp.size() > maxResp) {
				// Over the ceiling: this is not a "small body" — treat it as a
				// failure rather than silently handing back a truncated payload.
				resp.clear();
				break;
			}
		} else if (n == -2) {
			// recv timed out (no data this slice): keep waiting unless we've been
			// idle too long or were aborted.
			idleMs += 700;
			if (idleMs >= maxIdleMs)
				break;
		} else {
			break; // EOF (0) or real error (-1)
		}
	}
	tlsFree(tls);
	netClose(fd);
	return resp;
}

} // namespace

bool httpGet(const std::string& url, std::string& out, const std::atomic<bool>* abort) {
	out.clear();
	std::string current = url;
	for (int hop = 0; hop < 5 && !aborted(abort); hop++) { // follow up to 5 redirects
		Url u = parseUrl(current);
		if (!u.ok)
			return false;

		std::string resp = fetchOnce(u, abort);
		size_t headerEnd = resp.find("\r\n\r\n");
		if (headerEnd == std::string::npos)
			return false;

		std::string headers = resp.substr(0, headerEnd);
		std::string statusLine = headers.substr(0, headers.find("\r\n"));

		// Redirects: 301/302/303/307/308 with a Location header.
		if (statusLine.find(" 30") != std::string::npos) {
			std::string loc = headerValue(headers, "location");
			if (loc.empty())
				return false;
			current = resolveRedirect(u, loc);
			continue;
		}
		if (statusLine.find("200") == std::string::npos)
			return false;

		out = resp.substr(headerEnd + 4);
		return true;
	}
	return false; // too many redirects
}

} // namespace akaudio
