#include "Tls.hpp"

#include <sys/socket.h>

#include <openssl/ssl.h>

namespace akozlov {

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

long tlsRead(const Tls& t, int fd, void* buf, size_t n) {
	if (t.ssl)
		return SSL_read((SSL*) t.ssl, buf, (int) n);
	return (long) ::recv(fd, buf, n, 0);
}

long tlsWrite(const Tls& t, int fd, const void* buf, size_t n) {
	if (t.ssl)
		return SSL_write((SSL*) t.ssl, buf, (int) n);
	return (long) ::send(fd, buf, n, 0);
}

} // namespace akozlov
