// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "Tls.hpp"

#include <sys/socket.h>
#include <cerrno>

#include <openssl/ssl.h>

namespace akaudio {

bool tlsHandshake(Tls& t, int fd, const std::string& host) {
	// OpenSSL 1.1+ self-initializes on first use, so no explicit init needed.
	SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
	if (!ctx)
		return false;
	SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

	SSL* ssl = SSL_new(ctx);
	if (!ssl) {
		SSL_CTX_free(ctx);
		return false;
	}
	SSL_set_fd(ssl, fd);
	// SNI — most shared hosts need it to return the right certificate/vhost.
	SSL_set_tlsext_host_name(ssl, host.c_str());

	if (SSL_connect(ssl) != 1) {
		SSL_free(ssl);
		SSL_CTX_free(ctx);
		return false;
	}

	t.ssl = ssl;
	t.ctx = ctx;
	return true;
}

void tlsFree(Tls& t) {
	// Don't SSL_shutdown(): the peer socket may already be shut down by stop(),
	// and we don't care about a clean close_notify for a stream we're abandoning.
	if (t.ssl) {
		SSL_free((SSL*) t.ssl);
		t.ssl = nullptr;
	}
	if (t.ctx) {
		SSL_CTX_free((SSL_CTX*) t.ctx);
		t.ctx = nullptr;
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
	long r = (long) ::recv(fd, buf, n, 0);
	if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return -2;
	return r;
}

long tlsWrite(const Tls& t, int fd, const void* buf, size_t n) {
	if (t.ssl)
		return SSL_write((SSL*) t.ssl, buf, (int) n);
	return (long) ::send(fd, buf, n, 0);
}

} // namespace akaudio
