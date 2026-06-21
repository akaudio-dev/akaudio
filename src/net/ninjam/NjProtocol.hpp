#pragma once
// NINJAM wire protocol: message framing, the listen-only message subset, and the
// SHA1 auth helper. Pure (no sockets) so it is unit-testable on its own.
//
// Frame: [type u8][size u32 LE][payload]. Ints little-endian; strings NUL-terminated UTF-8.
// Ported from the canonical Cockos NINJAM source (justinfrankel/ninjam, GPLv2:
// netmsg.cpp/mpb.cpp) and cross-checked against JamTaba (GPLv2). Byte layouts verified
// against both. This plugin is GPL-3 (compatible). Credit: Cockos/NINJAM + JamTaba.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace akaudio {
namespace nj {

// Message type bytes (subset we use; full list in mpb.h).
enum MsgType : uint8_t {
	MSG_SERVER_AUTH_CHALLENGE   = 0x00,
	MSG_SERVER_AUTH_REPLY       = 0x01,
	MSG_SERVER_CONFIG_CHANGE    = 0x02,
	MSG_SERVER_USERINFO_CHANGE  = 0x03,
	MSG_SERVER_DOWNLOAD_BEGIN   = 0x04,
	MSG_SERVER_DOWNLOAD_WRITE   = 0x05,
	MSG_CLIENT_AUTH_USER        = 0x80,
	MSG_CLIENT_SET_USERMASK     = 0x81,
	MSG_CLIENT_SET_CHANNEL_INFO = 0x82,
	MSG_CLIENT_UPLOAD_BEGIN     = 0x83,
	MSG_CLIENT_UPLOAD_WRITE     = 0x84,
	MSG_CHAT                    = 0xc0,
	MSG_KEEPALIVE               = 0xfd,
};

static const uint32_t PROTO_VER_CUR = 0x00020000;
static const uint32_t PROTO_VER_MIN = 0x00020000;
static const uint32_t PROTO_VER_MAX = 0x0002ffff;

// ---- SHA1 (via the OpenSSL libRack exports; harness links libcrypto) -------------

// SHA1 of a..a+alen optionally followed by b..b+blen, into out[20].
void sha1(const void* a, size_t alen, const void* b, size_t blen, unsigned char out[20]);

// NINJAM auth: passhash = SHA1( SHA1(user ":" pass) + challenge8 ). Anonymous = empty pass.
void passHash(const std::string& user, const std::string& pass,
              const unsigned char challenge[8], unsigned char out[20]);

// ---- Little-endian byte cursor (bounds-checked read) -----------------------------

struct ByteReader {
	const uint8_t* p;
	const uint8_t* end;
	bool ok = true;

	ByteReader(const uint8_t* data, size_t len) : p(data), end(data + len) {}

	uint8_t u8() {
		if (p + 1 > end) { ok = false; return 0; }
		return *p++;
	}
	uint16_t u16() { uint16_t a = u8(); a |= (uint16_t)u8() << 8; return a; }
	uint32_t u32() {
		uint32_t a = u8(); a |= (uint32_t)u8() << 8;
		a |= (uint32_t)u8() << 16; a |= (uint32_t)u8() << 24; return a;
	}
	int8_t s8() { return (int8_t)u8(); }
	void bytes(void* dst, size_t n) {
		if (p + n > end) { ok = false; return; }
		std::memcpy(dst, p, n); p += n;
	}
	// NUL-terminated string up to end; advances past the NUL.
	std::string cstr() {
		const uint8_t* s = p;
		while (p < end && *p) p++;
		if (p >= end) { ok = false; return std::string(); }
		std::string out((const char*)s, (size_t)(p - s));
		p++; // skip NUL
		return out;
	}
	size_t remaining() const { return (size_t)(end - p); }
};

// ---- Parsed server messages ------------------------------------------------------

struct AuthChallenge {
	unsigned char challenge[8] = {0};
	uint32_t serverCaps = 0;
	uint32_t protoVer = 0;
	std::string license; // present iff serverCaps&1
	bool hasLicense = false;
	int keepAliveSecs() const { return (serverCaps >> 8) & 0xff; }
};
bool parseAuthChallenge(const uint8_t* data, size_t len, AuthChallenge& out);

struct AuthReply {
	uint8_t flag = 0;       // bit0 = success
	std::string errmsg;     // on success, an effective/updated username if set
	uint8_t maxchan = 0;
	bool success() const { return flag & 1; }
};
bool parseAuthReply(const uint8_t* data, size_t len, AuthReply& out);

struct ConfigChange {
	int bpm = 0;
	int bpi = 0;
};
bool parseConfigChange(const uint8_t* data, size_t len, ConfigChange& out);

// One (user, channel) record from USERINFO_CHANGE_NOTIFY.
struct UserChannel {
	bool active = false;
	int channelIdx = 0;
	int volumeDb10 = 0; // dB * 10 (0 = 0 dB)
	int pan = 0;        // -128..127
	int flags = 0;
	std::string user;
	std::string channel;
};
bool parseUserInfo(const uint8_t* data, size_t len, std::vector<UserChannel>& out);

// DOWNLOAD_INTERVAL_BEGIN: announces a new (user, channel) interval transfer.
struct DownloadBegin {
	unsigned char guid[16] = {0};
	uint32_t estSize = 0;
	uint32_t fourcc = 0; // 'OGGv' etc.; all-zero guid = silence
	int chidx = 0;
	std::string user;
};
bool parseDownloadBegin(const uint8_t* data, size_t len, DownloadBegin& out);

// DOWNLOAD_INTERVAL_WRITE: one chunk of an interval's encoded audio. `data`/`len`
// alias the caller's payload buffer (valid only during dispatch). flag bit0 = last.
struct DownloadWrite {
	unsigned char guid[16] = {0};
	uint8_t flags = 0;
	const uint8_t* data = nullptr;
	size_t len = 0;
	bool last() const { return flags & 1; }
};
bool parseDownloadWrite(const uint8_t* data, size_t len, DownloadWrite& out);

// ---- Builders: return a fully framed message ([type][size LE][payload]) ----------

std::vector<uint8_t> frame(uint8_t type, const std::vector<uint8_t>& payload);

std::vector<uint8_t> buildAuthUser(const std::string& username,
                                   const unsigned char passhash[20],
                                   uint32_t clientCaps, uint32_t clientVersion);

// SET_CHANNEL_INFO with a single inactive "filler" channel (listen-only: we
// broadcast nothing but still announce ourselves). Matches njclient's filler rec.
std::vector<uint8_t> buildSetChannelInfoListenOnly();

// SET_USERMASK: subscribe to a user's channels (bit N = channel N). Required to
// receive their interval audio on servers that don't auto-subscribe.
std::vector<uint8_t> buildSetUserMask(const std::string& user, uint32_t channelMask);

std::vector<uint8_t> buildKeepAlive();

} // namespace nj
} // namespace akaudio
