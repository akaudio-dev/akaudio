// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "NjClient.hpp"

#include <cerrno>
#include <cstring>
#include <ctime>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace akaudio {
namespace nj {

const char* stateName(NjClient::State s) {
	switch (s) {
		case NjClient::State::Idle: return "Idle";
		case NjClient::State::Connecting: return "Connecting";
		case NjClient::State::Authenticating: return "Authenticating";
		case NjClient::State::Connected: return "Connected";
		case NjClient::State::Error: return "Error";
		case NjClient::State::Stopped: return "Stopped";
		case NjClient::State::Disconnected: return "Disconnected";
	}
	return "?";
}

NjClient::~NjClient() {
	stop();
}

void NjClient::setState(State s, const std::string& msg) {
	st.store(s, std::memory_order_release);
	if (cb.onState) cb.onState(s, msg);
}

void NjClient::log(const std::string& msg) {
	if (cb.onLog) cb.onLog(msg);
}

void NjClient::start(const std::string& host, int port, const std::string& user,
                     const std::string& pass, Callbacks callbacks) {
	stop();
	cb = std::move(callbacks);
	abort.store(false, std::memory_order_release);
	running.store(true, std::memory_order_release);
	thread = std::thread(&NjClient::run, this, host, port, user, pass);
}

void NjClient::stop() {
	abort.store(true, std::memory_order_release);
	int fd = sock.load(std::memory_order_acquire);
	if (fd >= 0)
		::shutdown(fd, SHUT_RDWR); // interrupt a blocked recv; run() owns the close
	if (thread.joinable())
		thread.join();
	running.store(false, std::memory_order_release);
}

void NjClient::makeGuid(unsigned char out[16]) {
	static std::atomic<uint64_t> ctr{0};
	uint64_t a = ctr.fetch_add(1, std::memory_order_relaxed);
	uint64_t b = a * 0x9E3779B97F4A7C15ULL ^ 0xD1B54A32D192ED03ULL;
	std::memcpy(out, &a, 8);
	std::memcpy(out + 8, &b, 8);
}

void NjClient::setTransmit(const std::vector<std::string>& channelNames, float quality) {
	txChannels = channelNames;
	audio.setTransmit((int) channelNames.size(), quality);
	audio.onUploadInterval = [this](int ch, const std::vector<uint8_t>& ogg) {
		sendUploadInterval(ch, ogg);
	};
	if (st.load(std::memory_order_acquire) == State::Connected)
		sendChannelDecl(); // re-declare channels live
}

void NjClient::sendChannelDecl() {
	if (!txChannels.empty())
		sendAll(buildSetChannelInfo(txChannels));
	else
		sendAll(buildSetChannelInfoListenOnly());
}

void NjClient::sendUploadInterval(int chidx, const std::vector<uint8_t>& ogg) {
	unsigned char guid[16];
	makeGuid(guid);
	sendAll(buildUploadBegin(guid, FOURCC_OGG, chidx, (uint32_t) ogg.size()));
	const size_t CHUNK = 8192;
	size_t off = 0;
	do {
		size_t n = ogg.size() - off;
		if (n > CHUNK) n = CHUNK;
		bool last = (off + n >= ogg.size());
		sendAll(buildUploadWrite(guid, last ? 1 : 0, ogg.empty() ? nullptr : ogg.data() + off, n));
		off += n;
	} while (off < ogg.size()); // at least one WRITE (the last flag), even if ogg is empty
}

void NjClient::sendChat(const std::string& text) {
	// UI thread. sendAll is mutex-guarded and no-ops if the socket is gone, so a send
	// while disconnected is harmless. The server echoes our line back (with our name),
	// so we don't append locally. "!vote bpi/bpm N" rides this public path too.
	if (text.empty() || st.load(std::memory_order_acquire) != State::Connected)
		return;
	sendAll(buildChat({"MSG", text}));
}

void NjClient::sendAdmin(const std::string& command) {
	// Server admin command (topic/bpm/bpi/kick). The server enacts it only if we hold
	// admin rights; otherwise it is ignored. `command` is the text with the leading '/'
	// already stripped (e.g. "bpm 120"), matching njclient/JamTaba.
	if (command.empty() || st.load(std::memory_order_acquire) != State::Connected)
		return;
	sendAll(buildChat({"ADMIN", command}));
}

void NjClient::sendPrivate(const std::string& toUser, const std::string& text) {
	if (toUser.empty() || text.empty() || st.load(std::memory_order_acquire) != State::Connected)
		return;
	sendAll(buildChat({"PRIVMSG", toUser, text}));
}

bool NjClient::sendAll(const std::vector<uint8_t>& data) {
	std::lock_guard<std::mutex> lock(sendMutex); // whole NINJAM messages stay atomic on the wire
	int fd = sock.load(std::memory_order_acquire);
	if (fd < 0) return false;
	size_t off = 0;
	while (off < data.size()) {
		if (abort.load(std::memory_order_acquire)) return false;
		ssize_t n = ::send(fd, data.data() + off, data.size() - off, 0);
		if (n > 0) { off += (size_t)n; continue; }
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
		if (!abort.load(std::memory_order_acquire))
			log(std::string("send error: ") + std::strerror(errno));
		return false; // closed or error
	}
	return true;
}

int NjClient::recvExact(uint8_t* buf, size_t n, bool allowIdle) {
	int fd = sock.load(std::memory_order_acquire);
	if (fd < 0) {
		if (!abort.load(std::memory_order_acquire)) log("recvExact: socket invalid (fd<0)");
		return 0;
	}
	size_t got = 0;
	while (got < n) {
		if (abort.load(std::memory_order_acquire)) return 0;
		ssize_t r = ::recv(fd, buf + got, n - got, 0);
		if (r > 0) { got += (size_t)r; continue; }
		if (r == 0) {
			if (!abort.load(std::memory_order_acquire))
				log("disconnect: server closed connection (recv=0)");
			return 0; // peer closed
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			// SO_RCVTIMEO fired. At a frame boundary with nothing buffered, let the
			// caller service keepalives; mid-frame, keep waiting for the rest.
			if (allowIdle && got == 0) return -2;
			continue;
		}
		if (errno == EINTR) continue;
		if (!abort.load(std::memory_order_acquire))
			log(std::string("disconnect: recv error: ") + std::strerror(errno));
		return 0; // error
	}
	return 1;
}

int NjClient::recvFrame(uint8_t& type, std::vector<uint8_t>& payload) {
	uint8_t hdr[5];
	int h = recvExact(hdr, 5, /*allowIdle=*/true);
	if (h != 1) return h; // 0 = closed, -2 = idle
	type = hdr[0];
	uint32_t size = (uint32_t)hdr[1] | ((uint32_t)hdr[2] << 8) |
	                ((uint32_t)hdr[3] << 16) | ((uint32_t)hdr[4] << 24);
	payload.resize(size);
	if (size > 0) {
		int b = recvExact(payload.data(), size, /*allowIdle=*/false);
		if (b != 1) return 0;
	}
	return 1;
}

void NjClient::run(std::string host, int port, std::string user, std::string pass) {
	subMask.clear();
	setState(State::Connecting, host + ":" + std::to_string(port));

	// Resolve + connect (blocking), same pattern as StreamClient.
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* res = nullptr;
	if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
		setState(State::Error, "Cannot resolve host");
		running.store(false, std::memory_order_release);
		return;
	}
	int fd = -1;
	for (addrinfo* ai = res; ai; ai = ai->ai_next) {
		fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0) continue;
		if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
		::close(fd);
		fd = -1;
	}
	::freeaddrinfo(res);
	if (fd < 0) {
		setState(State::Error, "Connection failed");
		running.store(false, std::memory_order_release);
		return;
	}

	// TCP_NODELAY (small protocol messages) + a 1s recv timeout so we can service
	// keepalives without a second thread.
	int one = 1;
	::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	timeval tv{};
	tv.tv_sec = 1;
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	sock.store(fd, std::memory_order_release);

	setState(State::Authenticating);

	// Empty password = anonymous login ("anonymous[:displayname]"); a non-empty
	// password logs in as a registered user (username sent as-is).
	std::string wireUser;
	if (pass.empty()) {
		wireUser = "anonymous";
		if (!user.empty()) wireUser += ":" + user;
	} else {
		wireUser = user;
	}

	bool authed = false;
	time_t lastSend = ::time(nullptr);

	uint8_t type = 0;
	std::vector<uint8_t> payload;
	while (!abort.load(std::memory_order_acquire)) {
		// Send keepalive on a timer regardless of traffic. (When audio is flowing the
		// recv loop never goes idle, so this must NOT live only in the idle branch — else
		// we stop sending keepalives and the server drops us mid-stream.)
		time_t now = ::time(nullptr);
		int interval = keepAliveSecs > 0 ? keepAliveSecs : 3;
		if (now - lastSend >= interval) {
			if (!sendAll(buildKeepAlive())) {
				log("disconnect: keepalive send failed");
				break;
			}
			lastSend = now;
		}

		int r = recvFrame(type, payload);
		if (r == 0) break;  // closed / error (recvExact logs the real reason when not aborting)
		if (r == -2) continue; // idle (recv timed out); loop services keepalive above
		switch (type) {
			case MSG_SERVER_AUTH_CHALLENGE: {
				AuthChallenge ch;
				if (!parseAuthChallenge(payload.data(), payload.size(), ch)) {
					setState(State::Error, "Bad auth challenge");
					goto done;
				}
				if (ch.protoVer < PROTO_VER_MIN || ch.protoVer >= PROTO_VER_MAX)
					log("warning: server protocol version 0x" + std::to_string(ch.protoVer));
				keepAliveSecs = ch.keepAliveSecs();
				// log("challenge: caps=" + std::to_string(ch.serverCaps) +
				//     " keepalive=" + std::to_string(keepAliveSecs) + "s" +
				//     (ch.hasLicense ? " (license agreement present)" : "")); // normal

				unsigned char hash[20];
				passHash(wireUser, pass, ch.challenge, hash);
				// caps bit0 = accept license agreement (required if the server sent one);
				// JamTaba always sends caps=1. version present (caps high bits ignored by server).
				uint32_t caps = 1;
				if (!sendAll(buildAuthUser(wireUser, hash, caps, PROTO_VER_CUR))) {
					setState(State::Error, "Send auth failed");
					goto done;
				}
				lastSend = ::time(nullptr);
				break;
			}
			case MSG_SERVER_AUTH_REPLY: {
				AuthReply ar;
				parseAuthReply(payload.data(), payload.size(), ar);
				if (!ar.success()) {
					setState(State::Error, ar.errmsg.empty() ? "Auth rejected" : ar.errmsg);
					goto done;
				}
				authed = true;
				// log("auth ok" + (ar.errmsg.empty() ? std::string() : " as \"" + ar.errmsg + "\"") +
				//     " maxchan=" + std::to_string(ar.maxchan)); // normal
				// Announce our channels (real broadcast channels if transmitting, else a
				// listen-only filler).
				sendChannelDecl();
				lastSend = ::time(nullptr);
				audio.start(); // launch the interval mixer + transmit thread
				setState(State::Connected);
				break;
			}
			case MSG_SERVER_CONFIG_CHANGE: {
				ConfigChange cc;
				if (parseConfigChange(payload.data(), payload.size(), cc)) {
					audio.setTempo(cc.bpm, cc.bpi);
					if (cb.onConfig) cb.onConfig(cc.bpm, cc.bpi);
				}
				break;
			}
			case MSG_SERVER_USERINFO_CHANGE: {
				std::vector<UserChannel> users;
				if (parseUserInfo(payload.data(), payload.size(), users)) {
					for (const auto& u : users) {
						audio.onUserChannel(u.user, u.channelIdx, u.active, u.volumeDb10, u.pan);
						// Subscribe to this user's active channels (SET_USERMASK); many
						// servers don't auto-subscribe, so without this we get no audio.
						uint32_t& mask = subMask[u.user];
						uint32_t before = mask;
						if (u.active) mask |= (1u << u.channelIdx);
						else mask &= ~(1u << u.channelIdx);
						if (mask != before) {
							// log("subscribe " + u.user + " chanmask=" + std::to_string(mask)); // normal
							sendAll(buildSetUserMask(u.user, mask));
							lastSend = ::time(nullptr);
						}
					}
					if (cb.onUserInfo) cb.onUserInfo(users);
				}
				break;
			}
			case MSG_CHAT: {
				ChatMessage cm;
				if (parseChat(payload.data(), payload.size(), cm) && cb.onChat)
					cb.onChat(cm);
				break;
			}
			case MSG_SERVER_DOWNLOAD_BEGIN: {
				DownloadBegin db;
				if (parseDownloadBegin(payload.data(), payload.size(), db))
					audio.beginInterval(db.user, db.chidx, db.guid, db.fourcc);
				break;
			}
			case MSG_SERVER_DOWNLOAD_WRITE: {
				DownloadWrite dw;
				if (parseDownloadWrite(payload.data(), payload.size(), dw))
					audio.writeInterval(dw.guid, dw.data, dw.len, dw.last());
				break;
			}
			default:
				break;
		}
	}

done:
	(void)authed;
	audio.stop();
	int f = sock.exchange(-1, std::memory_order_acq_rel);
	if (f >= 0) ::close(f);
	// Terminal state, in priority order: a specific Error (auth/protocol failure) set via
	// `goto done` stands as-is; otherwise distinguish who ended the session. abort set => the
	// UI called stop() (user pressed Leave) => Stopped. abort clear => the loop fell out because
	// the socket closed under us — a NINJAM kick is exactly this (the server broadcasts a
	// "kicked by" chat MSG, then drops the TCP connection; there is no dedicated kick message),
	// as is a server shutdown or network loss => Disconnected so the UI can report it.
	if (st.load(std::memory_order_acquire) != State::Error)
		setState(abort.load(std::memory_order_acquire) ? State::Stopped
		                                               : State::Disconnected,
		         abort.load(std::memory_order_acquire) ? "" : "Disconnected by server");
	running.store(false, std::memory_order_release);
}

} // namespace nj
} // namespace akaudio
