// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "NjClient.hpp"

#include "../Socket.hpp"

#include <cstring>
#include <ctime>
#include <map>
#include <random>

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
	// (Re)bind the upload hook while no threads are running — assigning a
	// std::function that the TX thread may concurrently invoke would be a race,
	// so it must never happen in setTransmit() on a live session.
	audio.onUploadInterval = [this](int ch, const std::vector<uint8_t>& ogg) {
		sendUploadInterval(ch, ogg);
	};
	abort.store(false, std::memory_order_release);
	running.store(true, std::memory_order_release);
	thread = std::thread(&NjClient::run, this, host, port, user, pass);
}

void NjClient::stop() {
	abort.store(true, std::memory_order_release);
	// Interrupt a blocked recv; run() owns the close. GuardedFd makes this atomic
	// with that close so we can't shutdown() a recycled fd number.
	sock.shutdown();
	if (thread.joinable())
		thread.join();
	running.store(false, std::memory_order_release);
}

void NjClient::makeGuid(unsigned char out[16]) {
	// A per-process random seed mixed into every GUID. Receiving clients key their
	// transfer map by GUID alone (no username), so two akaudio users transmitting in
	// one room MUST NOT generate the same GUID sequence — a pure counter did, and
	// their interval chunks collided into one buffer (garbled/silent audio). Seeded
	// once from the OS RNG so the sequences diverge per install/session.
	static const uint64_t seed = [] {
		std::random_device rd;
		return ((uint64_t)rd() << 32) ^ rd() ^ 0xD1B54A32D192ED03ULL;
	}();
	static std::atomic<uint64_t> ctr{0};
	uint64_t a = seed + ctr.fetch_add(1, std::memory_order_relaxed);
	uint64_t b = a * 0x9E3779B97F4A7C15ULL ^ seed;
	std::memcpy(out, &a, 8);
	std::memcpy(out + 8, &b, 8);
}

void NjClient::setTransmit(const std::vector<std::string>& channelNames, float quality) {
	{
		// UI thread; the net thread reads txChannels in sendChannelDecl() (e.g.
		// on AUTH_REPLY), so the write must be guarded.
		std::lock_guard<std::mutex> lock(txMutex);
		txChannels = channelNames;
	}
	audio.setTransmit((int) channelNames.size(), quality);
	if (st.load(std::memory_order_acquire) == State::Connected)
		sendChannelDecl(); // re-declare channels live
}

void NjClient::sendChannelDecl() {
	std::vector<std::string> names;
	{
		std::lock_guard<std::mutex> lock(txMutex);
		names = txChannels;
	}
	if (!names.empty())
		sendAll(buildSetChannelInfo(names));
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

void NjClient::sendChatParts(const std::vector<std::string>& parts) {
	// UI thread. sendAll is mutex-guarded and no-ops if the socket is gone, so a send
	// while disconnected is harmless.
	if (st.load(std::memory_order_acquire) != State::Connected)
		return;
	sendAll(buildChat(parts));
}

void NjClient::sendChat(const std::string& text) {
	// The server echoes our line back (with our name), so we don't append locally.
	// "!vote bpi/bpm N" rides this public path too.
	if (!text.empty())
		sendChatParts({"MSG", text});
}

void NjClient::sendAdmin(const std::string& command) {
	// Server admin command (topic/bpm/bpi/kick). The server enacts it only if we hold
	// admin rights; otherwise it is ignored. `command` is the text with the leading '/'
	// already stripped (e.g. "bpm 120"), matching njclient/JamTaba.
	if (!command.empty())
		sendChatParts({"ADMIN", command});
}

void NjClient::sendPrivate(const std::string& toUser, const std::string& text) {
	if (!toUser.empty() && !text.empty())
		sendChatParts({"PRIVMSG", toUser, text});
}

bool NjClient::sendAll(const std::vector<uint8_t>& data) {
	std::lock_guard<std::mutex> lock(sendMutex); // whole NINJAM messages stay atomic on the wire
	int fd = sock.fd();
	if (fd < 0) return false;
	size_t off = 0;
	while (off < data.size()) {
		if (abort.load(std::memory_order_acquire)) return false;
		long n = (long) ::send(fd, (const char*) (data.data() + off), (int) (data.size() - off), 0);
		if (n > 0) { off += (size_t)n; continue; }
		if (n < 0 && (netWouldBlock() || netInterrupted())) continue;
		if (!abort.load(std::memory_order_acquire))
			log(std::string("send error: ") + netErrorStr());
		return false; // closed or error
	}
	return true;
}

int NjClient::recvExact(uint8_t* buf, size_t n, bool allowIdle) {
	int fd = sock.fd();
	if (fd < 0) {
		if (!abort.load(std::memory_order_acquire)) log("recvExact: socket invalid (fd<0)");
		return 0;
	}
	size_t got = 0;
	while (got < n) {
		if (abort.load(std::memory_order_acquire)) return 0;
		long r = (long) ::recv(fd, (char*) (buf + got), (int) (n - got), 0);
		if (r > 0) { got += (size_t)r; continue; }
		if (r == 0) {
			if (!abort.load(std::memory_order_acquire))
				log("disconnect: server closed connection (recv=0)");
			return 0; // peer closed
		}
		if (netWouldBlock()) {
			// SO_RCVTIMEO fired. At a frame boundary with nothing buffered, let the
			// caller service keepalives; mid-frame, keep waiting for the rest.
			if (allowIdle && got == 0) return -2;
			continue;
		}
		if (netInterrupted()) continue;
		if (!abort.load(std::memory_order_acquire))
			log(std::string("disconnect: recv error: ") + netErrorStr());
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
	// The size is an unvalidated wire field. A NINJAM message is never remotely this
	// large (the biggest is a ~8K interval DOWNLOAD_WRITE chunk); treat anything huge
	// as a corrupt/hostile frame and disconnect rather than trying to allocate up to
	// 4 GiB (which would throw bad_alloc → terminate the host).
	if (size > (16u << 20)) {
		log("disconnect: absurd frame size " + std::to_string(size));
		return 0;
	}
	payload.resize(size);
	if (size > 0) {
		int b = recvExact(payload.data(), size, /*allowIdle=*/false);
		if (b != 1) return 0;
	}
	return 1;
}

void NjClient::run(std::string host, int port, std::string user, std::string pass) {
	// Per-user subscribed-channel bitmask (SET_USERMASK); session-local by construction.
	std::map<std::string, uint32_t> subMask;
	setState(State::Connecting, host + ":" + std::to_string(port));

	// Resolve + connect, abort-pollable (netResolveConnect) — a blocking
	// connect() here would make stop() on a dead host hang the UI thread in
	// join() for the full OS connect timeout.
	std::string connErr;
	int fd = netResolveConnect(host, std::to_string(port), &abort, 8000, &connErr);
	if (fd < 0) {
		bool wasAborted = abort.load(std::memory_order_acquire);
		setState(wasAborted ? State::Stopped : State::Error, wasAborted ? "" : connErr);
		running.store(false, std::memory_order_release);
		return;
	}

	// TCP_NODELAY (small protocol messages) + a 1s recv timeout so we can service
	// keepalives without a second thread. A send timeout too: without it a wedged
	// peer (dropped link, no RST) with a full TCP send buffer blocks ::send inside
	// sendAll indefinitely — and since sendAll holds sendMutex, a UI-thread chat send
	// would then freeze the UI. The timeout turns that into a bounded would-block.
	netSetTcpNoDelay(fd);
	netSetRcvTimeout(fd, 1000);
	netSetSndTimeout(fd, 2000);
	sock.publish(fd);

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
				break;
			}
			case MSG_SERVER_AUTH_REPLY: {
				AuthReply ar;
				parseAuthReply(payload.data(), payload.size(), ar);
				if (!ar.success()) {
					setState(State::Error, ar.errmsg.empty() ? "Auth rejected" : ar.errmsg);
					goto done;
				}
				// log("auth ok" + (ar.errmsg.empty() ? std::string() : " as \"" + ar.errmsg + "\"") +
				//     " maxchan=" + std::to_string(ar.maxchan)); // normal
				// Announce our channels (real broadcast channels if transmitting, else a
				// listen-only filler).
				sendChannelDecl();
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
						// channelIdx is an unvalidated wire u8 — a shift by >= 32 is UB,
						// and the mask can't represent those channels anyway.
						if (u.channelIdx >= 32)
							continue;
						uint32_t& mask = subMask[u.user];
						uint32_t before = mask;
						if (u.active) mask |= (1u << u.channelIdx);
						else mask &= ~(1u << u.channelIdx);
						if (mask != before) {
							// log("subscribe " + u.user + " chanmask=" + std::to_string(mask)); // normal
							sendAll(buildSetUserMask(u.user, mask));
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
					audio.beginInterval(db.user, db.chidx, db.guid, db.fourcc, db.estSize);
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
	audio.stop();
	sock.closeOwned(); // atomic with stop()'s shutdown (see stop())
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
