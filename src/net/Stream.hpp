#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

#include "RingBuffer.hpp"

// Shared networked-audio layer for Akozlov modules (Ninjam, Radio, future modules).
//
// StreamClient connects to an HTTP/Icecast MP3 stream on a background thread,
// decodes it (dr_mp3), resamples to the engine sample rate, and feeds a lock-free
// ring buffer. Module::process() (audio thread) pulls one stereo frame at a time
// via pull() and must never block — all networking/decoding happens off-thread.
//
// Scope / limitations:
//   - http:// and https:// (TLS via the OpenSSL libRack exports; SNI on, but
//     certificate verification is not enforced — see net/Tls.hpp).
//   - .pls / .m3u playlist URLs are auto-resolved to the first stream URL.
//   - MP3 always; AAC on macOS (AudioToolbox). No OGG/Vorbis or HLS yet.
//   - linear resampling from the stream rate to the engine rate.

namespace akozlov {

class StreamClient {
public:
	enum class State { Stopped, Connecting, Playing, Error };

	StreamClient() = default;
	~StreamClient();

	StreamClient(const StreamClient&) = delete;
	StreamClient& operator=(const StreamClient&) = delete;

	// Engine sample rate; safe to call any time (e.g. onSampleRateChange).
	void setSampleRate(float sr) { sampleRate.store(sr, std::memory_order_relaxed); }

	// Start streaming the given URL (UI thread). Stops any current stream first.
	void start(const std::string& url);
	// Stop and join the background thread (UI thread).
	void stop();

	bool isRunning() const { return running.load(std::memory_order_acquire); }
	State getState() const { return state.load(std::memory_order_acquire); }
	std::string getStatusText();

	// Audio thread: fetch the next stereo frame. Returns false on underrun
	// (caller should output silence for this frame).
	bool pull(float& l, float& r) { return ring.pull(l, r); }

private:
	void run(std::string url);
	void setStatus(State s, const std::string& text);

	StereoRingBuffer ring{1 << 16}; // ~0.7 s at 48 kHz of headroom
	std::thread thread;
	std::atomic<bool> running{false};
	std::atomic<bool> abort{false};
	std::atomic<int> sock{-1}; // open socket fd, or -1; closed by stop() to interrupt recv
	std::atomic<float> sampleRate{44100.f};
	std::atomic<State> state{State::Stopped};

	std::mutex statusMutex;
	std::string statusText;
};

} // namespace akozlov
