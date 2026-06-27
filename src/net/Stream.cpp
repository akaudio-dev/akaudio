// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrey Kozlov

#include "Stream.hpp"

#include <cctype>
#include <chrono>
#include <cstring>
#include <vector>

#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>

#include "../dep/dr_mp3.h"
#include "Tls.hpp"
#include "AacDecoder.hpp"
#include "Http.hpp"
#include "Hls.hpp"

namespace akaudio {

namespace {

// Parsed http(s)://host[:port][/path]
struct Url {
	std::string host;
	std::string port = "80";
	std::string path = "/";
	bool tls = false;
	bool ok = false;
};

Url parseUrl(const std::string& url) {
	Url u;
	std::string s = url;
	if (s.rfind("https://", 0) == 0) {
		u.tls = true;
		u.port = "443";
		s = s.substr(8);
	}
	else if (s.rfind("http://", 0) == 0) {
		s = s.substr(7);
	}
	else {
		return u; // unsupported scheme
	}

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

bool endsWithCI(const std::string& s, const std::string& suffix) {
	if (s.size() < suffix.size())
		return false;
	for (size_t i = 0; i < suffix.size(); i++) {
		char a = (char) std::tolower((unsigned char) s[s.size() - suffix.size() + i]);
		char b = (char) std::tolower((unsigned char) suffix[i]);
		if (a != b)
			return false;
	}
	return true;
}

// Path part of a URL, without the query string (for extension checks).
std::string pathNoQuery(const std::string& url) {
	std::string p = parseUrl(url).path;
	size_t q = p.find('?');
	return q == std::string::npos ? p : p.substr(0, q);
}

// True if the URL points at a .pls or .m3u playlist (NOT .m3u8, which is HLS).
bool isPlaylistUrl(const std::string& url) {
	std::string p = pathNoQuery(url);
	return endsWithCI(p, ".pls") || endsWithCI(p, ".m3u");
}

// Extract the first http(s) media URL from a .pls or .m3u body. Works for both:
// PLS "FileN=http://…" lines and M3U bare-URL lines; '#' comment lines and the
// PLS "File1=" prefix are handled by scanning for the first http(s) token.
std::string firstPlaylistUrl(const std::string& body) {
	size_t i = 0, n = body.size();
	while (i < n) {
		size_t e = body.find('\n', i);
		std::string line = body.substr(i, (e == std::string::npos ? n : e) - i);
		i = (e == std::string::npos ? n : e + 1);

		size_t h = line.find("http://");
		if (h == std::string::npos)
			h = line.find("https://");
		if (h == std::string::npos)
			continue;
		std::string u = line.substr(h);
		while (!u.empty() && (u.back() == '\r' || u.back() == ' ' || u.back() == '\t'))
			u.pop_back();
		if (!u.empty())
			return u;
	}
	return "";
}

// Context handed to dr_mp3's read callback: serves the post-header leftover bytes
// first, then reads directly from the socket.
struct ReadCtx {
	StreamClient* self;
	int fd;
	const Tls* tls;
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

	long n = tlsRead(*ctx->tls, ctx->fd, pBufferOut, bytesToRead);
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
	produced.store(0, std::memory_order_release);
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
	// Resolve .pls / .m3u playlists to the actual stream URL (a few hops, in case
	// a playlist points at another). Blocking httpGet is fine on this background
	// thread. Leaves `url` unchanged for direct streams.
	for (int hop = 0; hop < 4 && isPlaylistUrl(url); hop++) {
		if (abort.load(std::memory_order_acquire))
			break;
		setStatus(State::Connecting, "Reading playlist\xe2\x80\xa6");
		std::string body;
		if (!httpGet(url, body, &abort))
			break;
		std::string next = firstPlaylistUrl(body);
		if (next.empty() || next == url)
			break;
		url = next;
	}

	// HLS (.m3u8) is segment-based, not a continuous stream — handle separately.
	if (looksLikeHls(url, "")) {
		runHls(url);
		running.store(false, std::memory_order_release);
		return;
	}

	Url u = parseUrl(url);
	if (!u.ok) {
		setStatus(State::Error, "Bad URL (need http:// or https://)");
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

	// Non-blocking connect that polls abort every 100 ms, so stop() (called on the
	// UI thread on every station switch) never blocks on a slow/dead host's connect
	// timeout — the cause of UI freezes when stepping quickly between stations.
	int fd = -1;
	for (addrinfo* ai = res; ai; ai = ai->ai_next) {
		int s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s < 0)
			continue;
#ifdef SO_NOSIGPIPE
		// Without this, a send() on a socket that stop() has shutdown() raises
		// SIGPIPE, which terminates the whole process (Rack doesn't ignore it) —
		// the crash seen when switching stations rapidly. Make writes return EPIPE.
		int nosig = 1;
		::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif
		sock.store(s, std::memory_order_release); // publish before connecting so stop() can see it

		int flags = ::fcntl(s, F_GETFL, 0);
		::fcntl(s, F_SETFL, flags | O_NONBLOCK);
		int rc = ::connect(s, ai->ai_addr, ai->ai_addrlen);
		bool connected = (rc == 0);
		if (rc < 0 && errno == EINPROGRESS) {
			for (int i = 0; i < 80 && !abort.load(std::memory_order_acquire); i++) {
				fd_set wf;
				FD_ZERO(&wf);
				FD_SET(s, &wf);
				timeval tv{0, 100 * 1000}; // 100 ms
				int sel = ::select(s + 1, nullptr, &wf, nullptr, &tv);
				if (sel > 0) {
					int err = 0;
					socklen_t len = sizeof(err);
					::getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
					connected = (err == 0);
					break;
				}
				if (sel < 0 && errno != EINTR)
					break;
			}
		}
		if (connected) {
			::fcntl(s, F_SETFL, flags); // restore blocking for the read loop
			fd = s;
			break;
		}
		sock.store(-1, std::memory_order_release);
		::close(s);
		if (abort.load(std::memory_order_acquire))
			break;
	}
	::freeaddrinfo(res);

	if (fd < 0) {
		bool aborted = abort.load(std::memory_order_acquire);
		setStatus(aborted ? State::Stopped : State::Error, aborted ? "Stopped" : "Connection failed");
		running.store(false, std::memory_order_release);
		return;
	}

	// For https, wrap the socket in TLS before any HTTP I/O. Declared here (before
	// the first `goto cleanup`) so it's in scope at the cleanup label, which frees
	// it. tlsRead/tlsWrite fall back to plain recv/send when tls is inactive (http).
	Tls tls;
	if (u.tls && !tlsHandshake(tls, fd, u.host)) {
		setStatus(State::Error, "TLS handshake failed");
		goto cleanup;
	}

	// Send the request. Icy-MetaData:0 keeps the MP3 body free of interleaved
	// metadata. Scoped in its own block so `req` isn't live at the cleanup label
	// (the goto above would otherwise bypass its initialization).
	{
		std::string req =
			"GET " + u.path + " HTTP/1.0\r\n"
			"Host: " + u.host + "\r\n"
			"User-Agent: AKAudio-VCVRack/2.0\r\n"
			"Icy-MetaData: 0\r\n"
			"Connection: close\r\n"
			"\r\n";
		if (tlsWrite(tls, fd, req.data(), req.size()) < 0) {
			setStatus(State::Error, "Send failed");
			goto cleanup;
		}
	}

	{
		// Read HTTP headers up to the blank line; keep any body bytes already read.
		std::string head;
		char tmp[2048];
		size_t headerEnd = std::string::npos;
		while (!abort.load(std::memory_order_acquire) && head.size() < (1 << 16)) {
			long n = tlsRead(tls, fd, tmp, sizeof(tmp));
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

		// Pick the decoder dynamically from the response headers. Icecast sends a
		// Content-Type (audio/mpeg, audio/aac, …); default to MP3 when unspecified.
		std::string hl = head.substr(0, headerEnd);
		for (char& c : hl)
			c = (char) std::tolower((unsigned char) c);
		const bool isAac = hl.find("audio/aac") != std::string::npos
			|| hl.find("audio/aacp") != std::string::npos
			|| hl.find("application/aac") != std::string::npos;

		const size_t bodyStart = headerEnd + 4;
		std::vector<char> leftover(head.begin() + bodyStart, head.end());

		// Push one resampled stereo frame, blocking briefly under backpressure so
		// we drain the socket at playback speed and never drop audio. Returns false
		// when we should stop. Shared by both decoders' resamplers.
		auto pushFrame = [&](float l, float r) -> bool {
			while (!ring.push(l, r)) {
				if (!running.load(std::memory_order_acquire) || abort.load(std::memory_order_acquire))
					return false;
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			produced.fetch_add(1, std::memory_order_acq_rel);
			return true;
		};

		if (isAac) {
#if defined(__APPLE__)
			// AAC via the system AudioToolbox (macOS). The decoder pushes decoded
			// PCM through onPCM; we linear-resample to the engine rate here.
			AacDecoder dec;
			double phase = 0.0;
			float prevL = 0.f, prevR = 0.f;
			bool havePrev = false;
			bool aborted = false;
			dec.onPCM = [&](const float* pcm, int frames, double srcRate) {
				double engineRate = sampleRate.load(std::memory_order_relaxed);
				if (engineRate < 1.0)
					engineRate = 44100.0;
				const double step = srcRate / engineRate;
				for (int i = 0; i < frames; i++) {
					float curL = pcm[2 * i + 0];
					float curR = pcm[2 * i + 1];
					if (!havePrev) {
						prevL = curL;
						prevR = curR;
						havePrev = true;
						continue;
					}
					while (phase < 1.0) {
						float outL = prevL + (curL - prevL) * (float) phase;
						float outR = prevR + (curR - prevR) * (float) phase;
						if (!pushFrame(outL, outR)) {
							aborted = true;
							return;
						}
						phase += step;
					}
					phase -= 1.0;
					prevL = curL;
					prevR = curR;
				}
			};

			if (!dec.init()) {
				setStatus(State::Error, "AAC init failed");
				goto cleanup;
			}
			setStatus(State::Playing, "Playing " + u.host);

			if (!leftover.empty())
				dec.feed(reinterpret_cast<const uint8_t*>(leftover.data()), leftover.size());

			char buf[4096];
			while (!aborted && running.load(std::memory_order_acquire) && !abort.load(std::memory_order_acquire)) {
				long n = tlsRead(tls, fd, buf, sizeof(buf));
				if (n <= 0)
					break;
				if (!dec.feed(reinterpret_cast<const uint8_t*>(buf), (size_t) n))
					break;
			}
			dec.close();
			if (!abort.load(std::memory_order_acquire))
				setStatus(State::Stopped, "Stream ended");
#else
			setStatus(State::Error, "AAC needs macOS");
			goto cleanup;
#endif
		}
		else {
			ReadCtx ctx;
			ctx.self = this;
			ctx.fd = fd;
			ctx.tls = &tls;
			ctx.abort = &abort;
			ctx.leftover.assign(leftover.begin(), leftover.end());

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

			bool stop = false;
			while (!stop && running.load(std::memory_order_acquire) && !abort.load(std::memory_order_acquire)) {
				drmp3_uint64 got = drmp3_read_pcm_frames_f32(&mp3, framesPerRead, pcm.data());
				if (got == 0)
					break; // stream ended

				double engineRate = sampleRate.load(std::memory_order_relaxed);
				if (engineRate < 1.0)
					engineRate = 44100.0;
				const double step = srcRate / engineRate; // source advance per output frame

				for (drmp3_uint64 i = 0; i < got && !stop; i++) {
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
						if (!pushFrame(outL, outR)) {
							stop = true;
							break;
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
	}

cleanup:
	tlsFree(tls);
	{
		int f = sock.exchange(-1, std::memory_order_acq_rel);
		if (f >= 0)
			::close(f);
	}
	running.store(false, std::memory_order_release);
}

void StreamClient::runHls(std::string url) {
	if (!AacDecoder::available()) {
		setStatus(State::Error, "HLS needs macOS");
		return;
	}

	// If this is a master playlist, resolve to a (the first) variant media URL.
	std::string mediaUrl = url;
	{
		std::string body;
		if (httpGet(url, body, &abort)) {
			HlsPlaylist pl = parseHlsPlaylist(body);
			if (pl.isMaster && !pl.variant.empty())
				mediaUrl = urlJoin(url, pl.variant);
		}
	}

	// AAC decoder → linear resample → ring, identical to the direct-AAC path.
	AacDecoder dec;
	double phase = 0.0;
	float prevL = 0.f, prevR = 0.f;
	bool havePrev = false;
	bool aborted = false;
	auto pushFrame = [&](float l, float r) -> bool {
		while (!ring.push(l, r)) {
			if (!running.load(std::memory_order_acquire) || abort.load(std::memory_order_acquire))
				return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		produced.fetch_add(1, std::memory_order_acq_rel);
		return true;
	};
	dec.onPCM = [&](const float* pcm, int frames, double srcRate) {
		double engineRate = sampleRate.load(std::memory_order_relaxed);
		if (engineRate < 1.0)
			engineRate = 44100.0;
		const double step = srcRate / engineRate;
		for (int i = 0; i < frames; i++) {
			float curL = pcm[2 * i + 0];
			float curR = pcm[2 * i + 1];
			if (!havePrev) {
				prevL = curL;
				prevR = curR;
				havePrev = true;
				continue;
			}
			while (phase < 1.0) {
				if (!pushFrame(prevL + (curL - prevL) * (float) phase, prevR + (curR - prevR) * (float) phase)) {
					aborted = true;
					return;
				}
				phase += step;
			}
			phase -= 1.0;
			prevL = curL;
			prevR = curR;
		}
	};
	if (!dec.init()) {
		setStatus(State::Error, "AAC init failed");
		return;
	}

	setStatus(State::Connecting, "Buffering\xe2\x80\xa6");
	bool playing = false;
	bool haveLast = false;
	uint64_t lastSeq = 0;

	while (running.load(std::memory_order_acquire) && !abort.load(std::memory_order_acquire) && !aborted) {
		std::string body;
		if (!httpGet(mediaUrl, body, &abort)) {
			setStatus(State::Error, "Playlist fetch failed");
			break;
		}
		HlsPlaylist pl = parseHlsPlaylist(body);

		int played = 0;
		for (size_t k = 0; k < pl.segments.size(); k++) {
			uint64_t seq = pl.mediaSequence + k;
			if (haveLast && seq <= lastSeq)
				continue; // already played
			if (!running.load(std::memory_order_acquire) || abort.load(std::memory_order_acquire))
				break;

			std::string seg;
			if (!httpGet(urlJoin(mediaUrl, pl.segments[k]), seg, &abort))
				continue;
			std::string adts;
			tsExtractAdts(reinterpret_cast<const uint8_t*>(seg.data()), seg.size(), adts);
			if (!playing) {
				setStatus(State::Playing, "Playing (HLS)");
				playing = true;
			}
			dec.feed(reinterpret_cast<const uint8_t*>(adts.data()), adts.size()); // paces via backpressure
			lastSeq = seq;
			haveLast = true;
			played++;
			if (aborted)
				break;
		}

		if (pl.endList)
			break; // VOD ended

		if (played == 0) {
			// No new segments yet; wait ~half a target-duration and re-poll.
			int ms = (int) (pl.targetDuration * 500.0);
			if (ms < 200)
				ms = 200;
			for (int slept = 0; slept < ms && running.load(std::memory_order_acquire) && !abort.load(std::memory_order_acquire); slept += 50)
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}

	dec.close();
	if (!abort.load(std::memory_order_acquire) && state.load(std::memory_order_acquire) != State::Error)
		setStatus(State::Stopped, "Stream ended");
}

} // namespace akaudio
