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
#include "NjArchive.hpp"
#include "../Socket.hpp" // GuardedFd

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
	// SET_CHANNEL_INFO if already connected. Empty list = listen-only. `voice` marks
	// the channels as NINJAM voice chat (flags bit 2): we upload rolling ~2 s
	// intervals immediately (no beat grid) and receivers play them live.
	void setTransmit(const std::vector<std::string>& channelNames, float quality,
	                 bool voice = false);
	// Audio thread: (re)align interval capture to the room grid at a beat boundary
	// (beatIndex of beatCount elapsed → that much of the interval prefills as silence).
	void armTransmit(int beatIndex, int beatCount) { audio.armTransmit(beatIndex, beatCount); }
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
	// True once any room audio has reached the mix this session (join-gap countdown).
	bool audioStarted() const { return audio.audioStarted(); }

	// ---- Wire archive (Recorder) ----
	// Start/stop the raw-interval archive (received players + our TX). Off-thread I/O;
	// the RX/TX byte hooks are always wired and no-op while stopped. UI thread.
	void startArchive(const std::string& dir, bool recordTx) {
		audio.setArchiveListening(true); // before start: the next interval carries bytes
		archive.start(dir, recordTx);
	}
	void stopArchive() {
		archive.stop();
		audio.setArchiveListening(false); // stop carrying wire bytes nobody will write
	}
	bool archiveRunning() const { return archive.running(); }
	std::string archiveDir() const { return archive.dir(); }
	long archiveIntervals() const { return archive.totalIntervals(); }
	long archiveBytes() const { return archive.totalBytes(); }
	std::vector<NjArchive::PlayerStat> archiveStatus() const { return archive.status(); }
	// Audio thread, every frame: publish the session-timeline position. The archive
	// stamps TX rows against it, and NjAudio derives the mix→session offset that
	// stamps each received interval's playout start (§7.3).
	void setArchiveSessionFrame(uint64_t sf) {
		archive.setSessionFrame(sf);
		audio.publishSession(sf);
	}

private:
	void run(std::string host, int port, std::string user, std::string pass);
	void setState(State s, const std::string& msg = "");
	void log(const std::string& msg);
	void sendChatParts(const std::vector<std::string>& parts); // shared connected-guard + build + send
	void sendChannelDecl();                               // SET_CHANNEL_INFO (real or filler)
	// Streamed upload (TX thread): BEGIN announces a fresh per-channel GUID, then the
	// interval's bytes follow as chunked WRITEs while it is still being captured; the
	// final chunk (last=true) closes it.
	void sendUploadBegin(int chidx);
	void sendUploadData(int chidx, const uint8_t* data, size_t len, bool last);
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
	GuardedFd sock; // stop() shutdown()s it to interrupt recv; run() closeOwned()s it
	std::atomic<State> st{State::Idle};

	Callbacks cb;
	NjAudio audio;
	NjArchive archive;                        // raw-interval wire archive (off-thread; no-op unless started)
	// TX thread: accumulate each channel's uploaded bytes so a whole interval reaches
	// the archive verbatim on its final chunk. TX thread only. `txArchWhole` marks
	// channels whose current interval was archived from its BEGIN — an archive that
	// starts mid-interval must skip to the next one, or its first row is a headerless
	// mid-stream Ogg slice nothing can decode (found 2026-08-25 via Live's
	// "could not be decoded using OggFLAC" on the first tx row).
	std::vector<uint8_t> txAccum[NjAudio::MAX_TX];
	bool txArchWhole[NjAudio::MAX_TX] = {};
	// Session frame at the interval's BEGIN — the archived row's capture-start stamp.
	// Recorded where the interval actually starts (TX thread), so it stays correct
	// even when the tempo (and interval length) changes while the interval is in
	// flight — back-computing "end − currentTempo().frames" did not.
	uint64_t txStartSf[NjAudio::MAX_TX] = {};
	// Archive generation at the interval's BEGIN: a disarm + re-arm mid-interval bumps
	// the generation, so the final chunk refuses the gapped accumulation (chunks that
	// arrived while stopped were skipped — the bytes are not a whole interval).
	uint32_t txArchGen[NjAudio::MAX_TX] = {};
	unsigned char txGuid[NjAudio::MAX_TX][16] = {}; // in-flight upload GUID per channel (TX thread only)
	std::vector<std::string> txChannels;     // declared local broadcast channels (names)
	bool txVoice = false;                     // declared channels are voice chat (guarded by txMutex)
	std::mutex sendMutex;                     // serialize socket sends across net + TX threads
	std::mutex txMutex;                       // guards txChannels (UI writes vs net-thread reads)
	int keepAliveSecs = 3; // from server caps; refreshed after the challenge
};

const char* stateName(NjClient::State s);

} // namespace nj
} // namespace akaudio
