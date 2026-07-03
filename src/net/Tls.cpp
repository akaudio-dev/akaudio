// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "Tls.hpp"

#include "Socket.hpp"

#include <openssl/ssl.h>

namespace akaudio {

// Process-wide client context, built once (C++11 magic static) and never freed —
// creating an SSL_CTX per connection is needless work, and OpenSSL 1.1+ supports
// SSL_new() on a shared context from multiple threads.
static SSL_CTX* sharedCtx() {
	static SSL_CTX* ctx = []() {
		// OpenSSL 1.1+ self-initializes on first use, so no explicit init needed.
		SSL_CTX* c = SSL_CTX_new(TLS_client_method());
		if (c)
			SSL_CTX_set_verify(c, SSL_VERIFY_NONE, nullptr);
		return c;
	}();
	return ctx;
}

bool tlsHandshake(Tls& t, int fd, const std::string& host) {
	SSL_CTX* ctx = sharedCtx();
	if (!ctx)
		return false;

	SSL* ssl = SSL_new(ctx);
	if (!ssl)
		return false;
	SSL_set_fd(ssl, fd);
	// SNI — most shared hosts need it to return the right certificate/vhost.
	SSL_set_tlsext_host_name(ssl, host.c_str());

	if (SSL_connect(ssl) != 1) {
		SSL_free(ssl);
		return false;
	}

	t.ssl = ssl;
	return true;
}

void tlsFree(Tls& t) {
	// Don't SSL_shutdown(): the peer socket may already be shut down by stop(),
	// and we don't care about a clean close_notify for a stream we're abandoning.
	if (t.ssl) {
		SSL_free((SSL*) t.ssl);
		t.ssl = nullptr;
	}
}

// Returns >0 bytes, 0 on EOF, -1 on a real error, and -2 on would-block/timeout
// (e.g. a socket with SO_RCVTIMEO) so the caller can poll an abort flag and retry
// instead of treating a timeout as EOF.
long tlsRead(const Tls& t, int fd, void* buf, size_t n) {
	if (t.ssl) {
		int r = SSL_read((SSL*) t.ssl, buf, (int) n);
		if (r > 0)
			return r;
		int err = SSL_get_error((SSL*) t.ssl, r);
		if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
			return -2; // timed out / would block
		if (err == SSL_ERROR_ZERO_RETURN)
			return 0; // clean TLS close
		return -1; // real error
	}
	long r = (long) ::recv(fd, (char*) buf, (int) n, 0);
	if (r < 0 && netWouldBlock())
		return -2;
	return r;
}

long tlsWrite(const Tls& t, int fd, const void* buf, size_t n) {
	if (t.ssl)
		return SSL_write((SSL*) t.ssl, buf, (int) n);
	return (long) ::send(fd, (const char*) buf, (int) n, 0);
}

} // namespace akaudio
