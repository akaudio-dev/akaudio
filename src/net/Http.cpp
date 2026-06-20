#include "Http.hpp"
#include "Tls.hpp"

#include <cstring>
#include <vector>

#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <netinet/in.h>

namespace akozlov {

namespace {

// Parsed http(s)://host[:port][/path] (mirrors Stream.cpp's parser, kept local
// so the two net helpers stay independent).
struct Url {
	std::string host;
	std::string port = "80";
	std::string path = "/";
	bool tls = false;
	bool ok = false;
};

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

} // namespace

bool httpGet(const std::string& url, std::string& out) {
	out.clear();
	Url u = parseUrl(url);
	if (!u.ok)
		return false;

	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* res = nullptr;
	if (::getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &res) != 0 || !res)
		return false;

	int fd = -1;
	for (addrinfo* ai = res; ai; ai = ai->ai_next) {
		fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		::close(fd);
		fd = -1;
	}
	::freeaddrinfo(res);
	if (fd < 0)
		return false;

	// For https, wrap the socket in TLS (falls back to plain recv/send for http).
	Tls tls;
	if (u.tls && !tlsHandshake(tls, fd, u.host)) {
		::close(fd);
		return false;
	}

	std::string req =
		"GET " + u.path + " HTTP/1.0\r\n"
		"Host: " + u.host + "\r\n"
		"User-Agent: Akozlov-VCVRack/2.0\r\n"
		"Accept: application/json\r\n"
		"Connection: close\r\n"
		"\r\n";
	if (tlsWrite(tls, fd, req.data(), req.size()) < 0) {
		tlsFree(tls);
		::close(fd);
		return false;
	}

	// Read the whole response (headers + body), cap to a sane size.
	std::string resp;
	char buf[4096];
	const size_t maxResp = 4 << 20; // 4 MiB ceiling
	for (;;) {
		long n = tlsRead(tls, fd, buf, sizeof(buf));
		if (n <= 0)
			break;
		resp.append(buf, n);
		if (resp.size() > maxResp)
			break;
	}
	tlsFree(tls);
	::close(fd);

	size_t headerEnd = resp.find("\r\n\r\n");
	if (headerEnd == std::string::npos)
		return false;

	std::string statusLine = resp.substr(0, resp.find("\r\n"));
	if (statusLine.find("200") == std::string::npos)
		return false;

	out = resp.substr(headerEnd + 4);
	return true;
}

} // namespace akozlov
