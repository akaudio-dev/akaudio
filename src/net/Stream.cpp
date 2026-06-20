#include "Stream.hpp"

#include <chrono>
#include <cstring>
#include <vector>

#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <netinet/in.h>

#include "../dep/dr_mp3.h"

namespace akozlov {

namespace {

// Parsed http://host[:port][/path]
struct Url {
	std::string host;
	std::string port = "80";
	std::string path = "/";
	bool ok = false;
};

Url parseUrl(const std::string& url) {
	Url u;
	std::string s = url;
	const std::string scheme = "http://";
	if (s.rfind(scheme, 0) != 0)
		return u; // only plain http:// supported in v1
	s = s.substr(scheme.size());

	size_t slash = s.find('/');
	std::string authority = (slash == std::string::npos) ? s : s.substr(0, slash);
	u.path = (slash == std::string::npos) ? "/" : s.substr(slash);

	size_t colon = authority.find(':');
	if (colon == std::string::npos) {
		u.host = authority;
	}
	else {
		u.host = authority.substr(0, colon);
		u.port = authority.substr(colon + 1);
	}
	u.ok = !u.host.empty();
	return u;
}

// Context handed to dr_mp3's read callback: serves the post-header leftover bytes
// first, then reads directly from the socket.
struct ReadCtx {
	StreamClient* self;
	int fd;
	std::vector<char> leftover;
	size_t leftoverPos = 0;
	const std::atomic<bool>* abort;
};

size_t onRead(void* pUserData, void* pBufferOut, size_t bytesToRead) {
	ReadCtx* ctx = static_cast<ReadCtx*>(pUserData);
	if (ctx->abort->load(std::memory_order_acquire))
		return 0;

	// Drain leftover body bytes captured while reading the HTTP headers.
	if (ctx->leftoverPos < ctx->leftover.size()) {
		size_t avail = ctx->leftover.size() - ctx->leftoverPos;
		size_t n = avail < bytesToRead ? avail : bytesToRead;
		std::memcpy(pBufferOut, ctx->leftover.data() + ctx->leftoverPos, n);
		ctx->leftoverPos += n;
		return n;
	}

	ssize_t n = ::recv(ctx->fd, pBufferOut, bytesToRead, 0);
	return n > 0 ? static_cast<size_t>(n) : 0;
}

} // namespace

StreamClient::~StreamClient() {
	stop();
}

void StreamClient::setStatus(State s, const std::string& text) {
	state.store(s, std::memory_order_release);
	std::lock_guard<std::mutex> lock(statusMutex);
	statusText = text;
}

std::string StreamClient::getStatusText() {
	std::lock_guard<std::mutex> lock(statusMutex);
	return statusText;
}

void StreamClient::start(const std::string& url) {
	stop();
	abort.store(false, std::memory_order_release);
	running.store(true, std::memory_order_release);
	ring.clear();
	setStatus(State::Connecting, "Connecting…");
	thread = std::thread(&StreamClient::run, this, url);
}

void StreamClient::stop() {
	abort.store(true, std::memory_order_release);
	running.store(false, std::memory_order_release);
	// Interrupt a blocked recv() by shutting the socket down; run() owns the close.
	int fd = sock.load(std::memory_order_acquire);
	if (fd >= 0)
		::shutdown(fd, SHUT_RDWR);
	if (thread.joinable())
		thread.join();
	ring.clear();
	if (state.load(std::memory_order_acquire) != State::Error)
		setStatus(State::Stopped, "Stopped");
}

void StreamClient::run(std::string url) {
	Url u = parseUrl(url);
	if (!u.ok) {
		setStatus(State::Error, "Bad URL (only http:// supported)");
		running.store(false, std::memory_order_release);
		return;
	}

	// Resolve + connect (blocking).
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* res = nullptr;
	if (::getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &res) != 0 || !res) {
		setStatus(State::Error, "Cannot resolve host");
		running.store(false, std::memory_order_release);
		return;
	}

	int fd = -1;
	for (addrinfo* ai = res; ai; ai = ai->ai_next) {
		fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		::close(fd);
		fd = -1;
	}
	::freeaddrinfo(res);

	if (fd < 0) {
		setStatus(State::Error, "Connection failed");
		running.store(false, std::memory_order_release);
		return;
	}
	sock.store(fd, std::memory_order_release);

	// Send the request. Icy-MetaData:0 keeps the MP3 body free of interleaved metadata.
	std::string req =
		"GET " + u.path + " HTTP/1.0\r\n"
		"Host: " + u.host + "\r\n"
		"User-Agent: Akozlov-VCVRack/2.0\r\n"
		"Icy-MetaData: 0\r\n"
		"Connection: close\r\n"
		"\r\n";
	if (::send(fd, req.data(), req.size(), 0) < 0) {
		setStatus(State::Error, "Send failed");
		goto cleanup;
	}

	{
		// Read HTTP headers up to the blank line; keep any body bytes already read.
		std::string head;
		char tmp[2048];
		size_t headerEnd = std::string::npos;
		while (!abort.load(std::memory_order_acquire) && head.size() < (1 << 16)) {
			ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
			if (n <= 0)
				break;
			head.append(tmp, n);
			headerEnd = head.find("\r\n\r\n");
			if (headerEnd != std::string::npos)
				break;
		}
		if (headerEnd == std::string::npos) {
			setStatus(State::Error, "No HTTP response");
			goto cleanup;
		}

		std::string statusLine = head.substr(0, head.find("\r\n"));
		if (statusLine.find("200") == std::string::npos) {
			setStatus(State::Error, "HTTP: " + statusLine);
			goto cleanup;
		}

		ReadCtx ctx;
		ctx.self = this;
		ctx.fd = fd;
		ctx.abort = &abort;
		size_t bodyStart = headerEnd + 4;
		ctx.leftover.assign(head.begin() + bodyStart, head.end());

		// Pass NULL seek/tell: this is a non-seekable live socket. With a seek
		// callback dr_mp3 tries to rewind the first 10 bytes after its ID3v2
		// probe and aborts init when the rewind fails (DRMP3_FALSE); with NULL
		// it falls through and lets the decoder re-sync to the next frame.
		drmp3 mp3;
		if (!drmp3_init(&mp3, onRead, nullptr, nullptr, nullptr, &ctx, nullptr)) {
			setStatus(State::Error, "Not an MP3 stream");
			goto cleanup;
		}

		const drmp3_uint32 channels = mp3.channels;
		const double srcRate = mp3.sampleRate > 0 ? (double) mp3.sampleRate : 44100.0;
		setStatus(State::Playing, "Playing " + u.host);

		// Linear resampler state (source -> engine rate).
		double phase = 0.0;
		float prevL = 0.f, prevR = 0.f;
		bool havePrev = false;

		const drmp3_uint64 framesPerRead = 1152;
		std::vector<float> pcm(framesPerRead * (channels ? channels : 2));

		while (running.load(std::memory_order_acquire) && !abort.load(std::memory_order_acquire)) {
			drmp3_uint64 got = drmp3_read_pcm_frames_f32(&mp3, framesPerRead, pcm.data());
			if (got == 0)
				break; // stream ended

			double engineRate = sampleRate.load(std::memory_order_relaxed);
			if (engineRate < 1.0)
				engineRate = 44100.0;
			const double step = srcRate / engineRate; // source advance per output frame

			for (drmp3_uint64 i = 0; i < got; i++) {
				float curL, curR;
				if (channels >= 2) {
					curL = pcm[i * channels + 0];
					curR = pcm[i * channels + 1];
				}
				else {
					curL = curR = pcm[i];
				}

				if (!havePrev) {
					prevL = curL;
					prevR = curR;
					havePrev = true;
					continue;
				}

				while (phase < 1.0) {
					float outL = prevL + (curL - prevL) * (float) phase;
					float outR = prevR + (curR - prevR) * (float) phase;
					// Backpressure: block (briefly) when the ring is full so we
					// read from the socket at playback speed and never drop audio.
					while (!ring.push(outL, outR)) {
						if (!running.load(std::memory_order_acquire) || abort.load(std::memory_order_acquire))
							break;
						std::this_thread::sleep_for(std::chrono::milliseconds(2));
					}
					phase += step;
				}
				phase -= 1.0;
				prevL = curL;
				prevR = curR;
			}
		}

		drmp3_uninit(&mp3);
		if (!abort.load(std::memory_order_acquire))
			setStatus(State::Stopped, "Stream ended");
	}

cleanup:
	{
		int f = sock.exchange(-1, std::memory_order_acq_rel);
		if (f >= 0)
			::close(f);
	}
	running.store(false, std::memory_order_release);
}

} // namespace akozlov
