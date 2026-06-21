#pragma once
#include <string>
#include <cstddef>

// Minimal TLS client wrapper over an already-connected socket fd, using the
// OpenSSL that libRack already exports (no new dependency). We encrypt and send
// SNI, but do NOT hard-fail on certificate verification — there is no bundled CA
// store here, and these are public audio streams, not authenticated endpoints.
// Tighten to real verification later if we ship a CA bundle.

namespace akaudio {

struct Tls {
	void* ssl = nullptr; // SSL*
	void* ctx = nullptr; // SSL_CTX*
	bool active() const { return ssl != nullptr; }
};

// Perform the TLS handshake with SNI=host over an already-connected fd. Returns
// true on success; on failure frees any partial state (caller still owns fd).
bool tlsHandshake(Tls& t, int fd, const std::string& host);

// Free the SSL/SSL_CTX objects. Does NOT close the fd (the caller owns it).
void tlsFree(Tls& t);

// recv/send that transparently use TLS when t.active(), else the plain socket.
// Return >0 bytes, 0 on EOF, <0 on error (same shape as recv/send).
long tlsRead(const Tls& t, int fd, void* buf, size_t n);
long tlsWrite(const Tls& t, int fd, const void* buf, size_t n);

} // namespace akaudio
