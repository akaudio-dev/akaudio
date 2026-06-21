#include "NjProtocol.hpp"

#include <openssl/evp.h>

namespace akaudio {
namespace nj {

// ---- SHA1 (OpenSSL EVP; non-deprecated one-shot via a context) -------------------

void sha1(const void* a, size_t alen, const void* b, size_t blen, unsigned char out[20]) {
	EVP_MD_CTX* ctx = EVP_MD_CTX_new();
	EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
	if (a && alen) EVP_DigestUpdate(ctx, a, alen);
	if (b && blen) EVP_DigestUpdate(ctx, b, blen);
	unsigned int n = 0;
	EVP_DigestFinal_ex(ctx, out, &n);
	EVP_MD_CTX_free(ctx);
}

void passHash(const std::string& user, const std::string& pass,
              const unsigned char challenge[8], unsigned char out[20]) {
	// inner = SHA1(user ":" pass)
	std::string up = user + ":" + pass;
	unsigned char inner[20];
	sha1(up.data(), up.size(), nullptr, 0, inner);
	// out = SHA1(inner + challenge8)
	sha1(inner, sizeof(inner), challenge, 8, out);
}

// ---- Parsers ---------------------------------------------------------------------

bool parseAuthChallenge(const uint8_t* data, size_t len, AuthChallenge& out) {
	// challenge[8] + caps u32 + protoVer u32 (+ optional license string)
	if (len < 8 + 4 + 4) return false;
	ByteReader r(data, len);
	r.bytes(out.challenge, 8);
	out.serverCaps = r.u32();
	out.protoVer = r.u32();
	if (out.serverCaps & 1) {
		out.license = r.cstr();
		out.hasLicense = true;
	}
	return r.ok;
}

bool parseAuthReply(const uint8_t* data, size_t len, AuthReply& out) {
	if (len < 1) return false;
	ByteReader r(data, len);
	out.flag = r.u8();
	if (len > 1) {
		out.errmsg = r.cstr();      // NUL-terminated
		if (r.remaining() >= 1)
			out.maxchan = r.u8();
	}
	return true; // errmsg/maxchan are optional; only the flag is required
}

bool parseConfigChange(const uint8_t* data, size_t len, ConfigChange& out) {
	if (len < 4) return false;
	ByteReader r(data, len);
	out.bpm = r.u16();
	out.bpi = r.u16();
	return r.ok;
}

bool parseUserInfo(const uint8_t* data, size_t len, std::vector<UserChannel>& out) {
	// Repeated records: active u8, chidx u8, vol s16, pan s8, flags u8, user\0, chname\0
	ByteReader r(data, len);
	while (r.remaining() >= 6 + 2) { // header(6) + two NULs minimum
		UserChannel uc;
		uc.active = r.u8() != 0;
		uc.channelIdx = r.u8();
		uc.volumeDb10 = (int16_t)r.u16();
		uc.pan = r.s8();
		uc.flags = r.u8();
		uc.user = r.cstr();
		uc.channel = r.cstr();
		if (!r.ok) break;
		out.push_back(std::move(uc));
	}
	return true;
}

// ---- Builders --------------------------------------------------------------------

std::vector<uint8_t> frame(uint8_t type, const std::vector<uint8_t>& payload) {
	std::vector<uint8_t> f;
	f.reserve(5 + payload.size());
	uint32_t sz = (uint32_t)payload.size();
	f.push_back(type);
	f.push_back(sz & 0xff);
	f.push_back((sz >> 8) & 0xff);
	f.push_back((sz >> 16) & 0xff);
	f.push_back((sz >> 24) & 0xff);
	f.insert(f.end(), payload.begin(), payload.end());
	return f;
}

std::vector<uint8_t> buildAuthUser(const std::string& username,
                                   const unsigned char passhash[20],
                                   uint32_t clientCaps, uint32_t clientVersion) {
	// passhash[20] + username\0 + caps u32 + version u32
	std::vector<uint8_t> p;
	p.insert(p.end(), passhash, passhash + 20);
	p.insert(p.end(), username.begin(), username.end());
	p.push_back(0);
	for (int i = 0; i < 4; i++) p.push_back((clientCaps >> (8 * i)) & 0xff);
	for (int i = 0; i < 4; i++) p.push_back((clientVersion >> (8 * i)) & 0xff);
	return frame(MSG_CLIENT_AUTH_USER, p);
}

std::vector<uint8_t> buildSetChannelInfoListenOnly() {
	// mpisize u16 (=4), then one filler channel: name "\0", vol u16=0, pan u8=0, flags u8=0x80.
	std::vector<uint8_t> p;
	p.push_back(4); p.push_back(0); // mpisize = 4
	p.push_back(0);                 // empty channel name + NUL
	p.push_back(0); p.push_back(0); // volume s16 = 0
	p.push_back(0);                 // pan = 0
	p.push_back(0x80);              // flags = 0x80 (filler / inactive)
	return frame(MSG_CLIENT_SET_CHANNEL_INFO, p);
}

std::vector<uint8_t> buildKeepAlive() {
	return frame(MSG_KEEPALIVE, {});
}

} // namespace nj
} // namespace akaudio
