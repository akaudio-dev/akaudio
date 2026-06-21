#pragma once
// NjClient — a NINJAM protocol client (TCP, default port 2049) on a background thread.
//
// Phases 1-2 (this file): connect, anonymous SHA1 auth, keepalive, and parse the live
// room metadata (CONFIG_CHANGE = tempo, USERINFO_CHANGE = roster). It does NOT yet
// decode the interval audio (Phase 3) — that adds OGG reassembly + stb_vorbis + a ring
// buffer, mirroring StreamClient. This is a separate protocol from StreamClient (which
// only listens to a room's public Icecast mix); NjClient owns its own socket.
//
// Threading: start()/stop() are called from the UI/main thread. All socket I/O runs on
// the background thread; callbacks fire on that thread (the caller must marshal to the
// UI thread / use atomics as needed). stop() aborts via socket shutdown then joins.
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "NjProtocol.hpp"

namespace akaudio {
namespace nj {

class NjClient {
public:
	enum class State { Idle, Connecting, Authenticating, Connected, Error, Stopped };

	struct Callbacks {
		std::function<void(State, const std::string&)> onState;    // state transitions (+ message)
		std::function<void(int bpm, int bpi)> onConfig;            // CONFIG_CHANGE
		std::function<void(const std::vector<UserChannel>&)> onUserInfo; // USERINFO_CHANGE
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

private:
	void run(std::string host, int port, std::string user, std::string pass);
	void setState(State s, const std::string& msg = "");
	void log(const std::string& msg);

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
	int keepAliveSecs = 3; // from server caps; refreshed after the challenge
};

const char* stateName(NjClient::State s);

} // namespace nj
} // namespace akaudio
