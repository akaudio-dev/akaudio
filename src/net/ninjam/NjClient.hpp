// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
// NjClient — a full NINJAM protocol client (TCP, default port 2049) on a background
// thread: connect, anonymous/registered SHA1 auth, keepalive, live room metadata
// (CONFIG_CHANGE = tempo, USERINFO_CHANGE = roster), interval audio download +
// decode + per-player mix (NjAudio), TRANSMIT (local capture → OGG-Vorbis upload),
// and room chat. This is a separate protocol from StreamClient (which only listens
// to a room's public Icecast mix); NjClient owns its own socket.
//
// Threading: start()/stop() are called from the UI/main thread. All socket I/O runs on
// the background thread; callbacks fire on that thread (the caller must marshal to the
// UI thread / use atomics as needed). stop() aborts via socket shutdown then joins.
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "NjProtocol.hpp"
#include "NjAudio.hpp"

namespace akaudio {
namespace nj {

class NjClient {
public:
	// Stopped = we initiated the teardown (user pressed Leave). Disconnected = the server
	// closed the connection out from under us (kick, server shutdown, network loss); the UI
	// distinguishes the two so it can report an unexpected drop. Error = auth/protocol failure.
	enum class State { Idle, Connecting, Authenticating, Connected, Error, Stopped, Disconnected };

	struct Callbacks {
		std::function<void(State, const std::string&)> onState;    // state transitions (+ message)
		std::function<void(int bpm, int bpi)> onConfig;            // CONFIG_CHANGE
		std::function<void(const std::vector<UserChannel>&)> onUserInfo; // USERINFO_CHANGE
		std::function<void(const ChatMessage&)> onChat;            // CHAT_MESSAGE (any command)
		std::function<void(const std::string&)> onLog;             // optional debug log
	};

	NjClient() = default;
	~NjClient();

	NjClient(const NjClient&) = delete;
	NjClient& operator=(const NjClient&) = delete;

	// Connect + auth on a background thread. anonymous = empty pass; the wire username
	// becomes "anonymous:<user>" (or "anonymous" if user is empty). Stops any prior session.
	void start(const std::string& host, int port, const std::string& user,
	           const std::string& pass, Callbacks cb);
	// Abort + join the background thread (UI thread).
	void stop();

	State state() const { return st.load(std::memory_order_acquire); }
	bool isRunning() const { return running.load(std::memory_order_acquire); }

	// Engine sample rate for the interval mixer (UI/any thread).
	void setSampleRate(double sr) { audio.setSampleRate(sr); }
	// Audio thread: pull one wide frame (RING_CH per-slot-stereo floats). false on underrun.
	bool pullFrame(float* out) { return audio.pullFrame(out); }
	// Convenience stereo master pull (sum of slots) — used by the standalone harness.
	bool pull(float& l, float& r) { return audio.pull(l, r); }
	// Poly channel count currently in use (= number of active players on the bundle).
	int polyChannels() const { return audio.polyChannels(); }

	// ---- Transmit ----
	// Declare local broadcast channels (by name) + encoder quality; (re)sends
	// SET_CHANNEL_INFO if already connected. Empty list = listen-only.
	void setTransmit(const std::vector<std::string>& channelNames, float quality);
	// Audio thread: push one captured stereo frame for local channel `ch`.
	void captureFrame(int ch, float l, float r) { audio.captureFrame(ch, l, r); }

	// Send chat to the room (UI thread). No-op unless connected.
	void sendChat(const std::string& text);                 // public "MSG" (also carries "!vote …")
	void sendAdmin(const std::string& command);             // "ADMIN" (e.g. "bpm 120"); server acts only if you're admin
	void sendPrivate(const std::string& toUser, const std::string& text); // "PRIVMSG"

	// Diagnostics.
	long intervalsDecoded() const { return audio.intervalsDecoded(); }
	long decodeErrors() const { return audio.decodeErrors(); }
	long missedIntervals() const { return audio.missedIntervals(); }

private:
	void run(std::string host, int port, std::string user, std::string pass);
	void setState(State s, const std::string& msg = "");
	void log(const std::string& msg);
	void sendChatParts(const std::vector<std::string>& parts); // shared connected-guard + build + send
	void sendChannelDecl();                               // SET_CHANNEL_INFO (real or filler)
	void sendUploadInterval(int chidx, const std::vector<uint8_t>& ogg); // TX thread
	static void makeGuid(unsigned char out[16]);

	bool sendAll(const std::vector<uint8_t>& data);
	// Read one framed message. Returns: 1 = got frame, 0 = closed/error/abort,
	// -2 = idle (no frame ready, allows keepalive servicing).
	int recvFrame(uint8_t& type, std::vector<uint8_t>& payload);
	// Read exactly n bytes. allowIdle lets it return -2 when nothing has arrived yet
	// (used only at frame boundaries so keepalives can be sent).
	int recvExact(uint8_t* buf, size_t n, bool allowIdle);

	std::thread thread;
	std::atomic<bool> running{false};
	std::atomic<bool> abort{false};
	std::atomic<int> sock{-1};
	std::atomic<State> st{State::Idle};

	Callbacks cb;
	NjAudio audio;
	std::vector<std::string> txChannels;     // declared local broadcast channels (names)
	std::mutex sendMutex;                     // serialize socket sends across net + TX threads
	std::mutex sockMutex;                     // makes stop()'s shutdown atomic with run()'s close
	std::mutex txMutex;                       // guards txChannels (UI writes vs net-thread reads)
	int keepAliveSecs = 3; // from server caps; refreshed after the challenge
};

const char* stateName(NjClient::State s);

} // namespace nj
} // namespace akaudio
