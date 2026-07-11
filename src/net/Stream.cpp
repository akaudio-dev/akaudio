// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "Stream.hpp"

#include "Socket.hpp"

#include <cctype>
#include <chrono>
#include <cstring>
#include <vector>

#include "../dep/dr_mp3.h"
#include "Tls.hpp"
#include "AacDecoder.hpp"
#include "Http.hpp"
#include "Hls.hpp"

namespace akaudio {

namespace {

// URL parsing + endsWithCI live in Http.{hpp,cpp} (one parser shared by the
// whole net/ layer).

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

// The stream socket carries SO_RCVTIMEO so no recv can block forever; tlsRead
// reports each expired slice as -2. kIdleBudgetMs bounds how long we keep
// retrying with no data at all — a "live" stream silent that long is dead, and
// without this bound a stalled-but-open server pinned the thread in recv with
// the status stuck on "Playing" until the user hit stop.
constexpr int kRecvSliceMs = 2000;
constexpr int kIdleBudgetMs = 30000;

// Bounded read: data / EOF / error / abort / idle-budget-exhausted (the last two
// return 0, i.e. treated as end of stream).
long readIdle(const Tls& tls, int fd, void* buf, size_t n, const std::atomic<bool>& abort) {
	int idleMs = 0;
	for (;;) {
		if (abort.load(std::memory_order_acquire))
			return 0;
		long r = tlsRead(tls, fd, buf, n);
		if (r != -2)
			return r; // >0 data, 0 EOF, -1 error
		idleMs += kRecvSliceMs;
		if (idleMs >= kIdleBudgetMs)
			return 0;
	}
}

// Shared decode→ring plumbing for all three decode paths (MP3, AAC, HLS): the
// warm-up linear resampler (source rate → engine rate) plus the blocking
// backpressure push that paces the socket at playback speed. Built inside
// StreamClient's member functions with references to its state.
struct ResampleCtx {
	StereoRingBuffer& ring;
	const std::atomic<bool>& running;
	const std::atomic<bool>& abort;
	std::atomic<uint64_t>& produced;
	const std::atomic<float>& sampleRate;

	ResampleCtx(StereoRingBuffer& ring, const std::atomic<bool>& running,
			const std::atomic<bool>& abort, std::atomic<uint64_t>& produced,
			const std::atomic<float>& sampleRate)
		: ring(ring), running(running), abort(abort), produced(produced), sampleRate(sampleRate) {}

	// Linear resampler state.
	double phase = 0.0;
	float prevL = 0.f, prevR = 0.f;
	bool havePrev = false;
	bool stopped = false; // a push was refused → stop()/abort in progress

	// Source samples to advance per output frame.
	double stepFor(double srcRate) const {
		double engineRate = sampleRate.load(std::memory_order_relaxed);
		if (engineRate < 1.0)
			engineRate = 44100.0;
		return srcRate / engineRate;
	}

	// Push one output frame, blocking briefly under backpressure so the producer
	// reads at playback speed and never drops audio. False = stop.
	bool push(float l, float r) {
		while (!ring.push(l, r)) {
			if (!running.load(std::memory_order_acquire) || abort.load(std::memory_order_acquire)) {
				stopped = true;
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		produced.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	// Feed one source frame through the resampler (first frame just warms up
	// prev). False = stop requested; the caller should bail out.
	bool feed(float curL, float curR, double step) {
		if (!havePrev) {
			prevL = curL;
			prevR = curR;
			havePrev = true;
			return true;
		}
		while (phase < 1.0) {
			if (!push(prevL + (curL - prevL) * (float) phase,
			          prevR + (curR - prevR) * (float) phase))
				return false;
			phase += step;
		}
		phase -= 1.0;
		prevL = curL;
		prevR = curR;
		return true;
	}

	// Feed a block of interleaved stereo PCM (the AAC/HLS onPCM shape).
	void feedStereoBlock(const float* pcm, int frames, double srcRate) {
		const double step = stepFor(srcRate);
		for (int i = 0; i < frames; i++)
			if (!feed(pcm[2 * i + 0], pcm[2 * i + 1], step))
				return;
	}
};

// Context handed to dr_mp3's read callback: serves the post-header leftover bytes
// first, then reads directly from the socket (idle-bounded).
struct ReadCtx {
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

	long n = readIdle(*ctx->tls, ctx->fd, pBufferOut, bytesToRead, *ctx->abort);
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
	ring.requestClear(); // audio thread drops the old stream's tail on its next pull
	setStatus(State::Connecting, "Connecting…");
	thread = std::thread(&StreamClient::run, this, url);
}

void StreamClient::stop() {
	abort.store(true, std::memory_order_release);
	running.store(false, std::memory_order_release);
	// Interrupt a blocked recv() by shutting the socket down; run() owns the
	// close. sockMutex makes shutdown atomic with that close — otherwise we
	// could load the fd, lose the race to run()'s netClose, and shutdown() an fd
	// number the OS had already recycled for an unrelated connection.
	{
		std::lock_guard<std::mutex> lock(sockMutex);
		int fd = sock.load(std::memory_order_acquire);
		if (fd >= 0)
			netShutdown(fd);
	}
	if (thread.joinable())
		thread.join();
	ring.requestClear();
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
	if (looksLikeHls(url)) {
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

	// Resolve + connect. netConnectAbortable polls `abort` every 100 ms, so
	// stop() (called on the UI thread on every station switch) never blocks on a
	// slow/dead host's connect timeout — the old cause of UI freezes when
	// stepping quickly between stations.
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* res = nullptr;
	if (::getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &res) != 0 || !res) {
		setStatus(State::Error, "Cannot resolve host");
		running.store(false, std::memory_order_release);
		return;
	}
	int fd = netConnectAbortable(res, &abort, 8000);
	::freeaddrinfo(res);

	if (fd < 0) {
		bool aborted = abort.load(std::memory_order_acquire);
		setStatus(aborted ? State::Stopped : State::Error, aborted ? "Stopped" : "Connection failed");
		running.store(false, std::memory_order_release);
		return;
	}
	// Publish so stop() can interrupt a blocked recv; bound each recv so a
	// silent-but-open stream is noticed (readIdle retries -2 up to its budget).
	sock.store(fd, std::memory_order_release);
	netSetRcvTimeout(fd, kRecvSliceMs);

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
			long n = readIdle(tls, fd, tmp, sizeof(tmp), abort);
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

		// Decode → resample → blocking ring push, shared by both decoders.
		ResampleCtx rs{ring, running, abort, produced, sampleRate};

		if (isAac) {
#if defined(__APPLE__)
			// AAC via the system AudioToolbox (macOS). The decoder pushes decoded
			// PCM through onPCM; ResampleCtx resamples to the engine rate.
			AacDecoder dec;
			dec.onPCM = [&](const float* pcm, int frames, double srcRate) {
				rs.feedStereoBlock(pcm, frames, srcRate);
			};

			if (!dec.init()) {
				setStatus(State::Error, "AAC init failed");
				goto cleanup;
			}
			setStatus(State::Playing, "Playing " + u.host);

			if (!leftover.empty())
				dec.feed(reinterpret_cast<const uint8_t*>(leftover.data()), leftover.size());

			char buf[4096];
			while (!rs.stopped && running.load(std::memory_order_acquire) && !abort.load(std::memory_order_acquire)) {
				long n = readIdle(tls, fd, buf, sizeof(buf), abort);
				if (n <= 0)
					break;
				if (!dec.feed(reinterpret_cast<const uint8_t*>(buf), (size_t) n))
					break;
			}
			dec.close();
#else
			setStatus(State::Error, "AAC needs macOS");
			goto cleanup;
#endif
		}
		else {
			ReadCtx ctx;
			ctx.fd = fd;
			ctx.tls = &tls;
			ctx.abort = &abort;
			ctx.leftover = std::move(leftover); // branches are exclusive; AAC never gets here

			// Pass NULL seek/tell: this is a non-seekable live socket. With a seek
			// callback dr_mp3 tries to rewind the first 10 bytes after its ID3v2
			// probe and aborts init when the rewind fails (DRMP3_FALSE); with NULL
			// it falls through and lets the decoder re-sync to the next frame.
			drmp3 mp3;
			if (!drmp3_init(&mp3, onRead, nullptr, nullptr, nullptr, &ctx, nullptr)) {
				setStatus(State::Error, "Not an MP3 stream");
				goto cleanup;
			}

			setStatus(State::Playing, "Playing " + u.host);

			const drmp3_uint64 framesPerRead = 1152;
			// MP3 is at most 2 channels; always size for stereo so a mono→stereo
			// change mid-stream can never overflow the buffer.
			std::vector<float> pcm(framesPerRead * 2);

			while (!rs.stopped && running.load(std::memory_order_acquire) && !abort.load(std::memory_order_acquire)) {
				drmp3_uint64 got = drmp3_read_pcm_frames_f32(&mp3, framesPerRead, pcm.data());
				if (got == 0)
					break; // stream ended

				// dr_mp3 updates channels/sampleRate as frames decode; re-read
				// both per block so a mid-stream format change keeps the right
				// interleave and pitch (streams may switch, e.g. mono↔stereo).
				const drmp3_uint32 channels = mp3.channels ? mp3.channels : 2;
				const double srcRate = mp3.sampleRate > 0 ? (double) mp3.sampleRate : 44100.0;
				const double step = rs.stepFor(srcRate);

				for (drmp3_uint64 i = 0; i < got; i++) {
					float curL, curR;
					if (channels >= 2) {
						curL = pcm[i * channels + 0];
						curR = pcm[i * channels + 1];
					}
					else {
						curL = curR = pcm[i];
					}
					if (!rs.feed(curL, curR, step))
						break;
				}
			}

			drmp3_uninit(&mp3);
		}
		// Shared epilogue for both decode paths: the loop fell out because the stream
		// ended (on abort, stop() owns the terminal status).
		if (!abort.load(std::memory_order_acquire))
			setStatus(State::Stopped, "Stream ended");
	}

cleanup:
	tlsFree(tls);
	{
		// Atomic with stop()'s shutdown (see stop()).
		std::lock_guard<std::mutex> lock(sockMutex);
		int f = sock.exchange(-1, std::memory_order_acq_rel);
		if (f >= 0)
			netClose(f);
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

	// AAC decoder → ResampleCtx → ring, identical to the direct-AAC path.
	AacDecoder dec;
	ResampleCtx rs{ring, running, abort, produced, sampleRate};
	dec.onPCM = [&](const float* pcm, int frames, double srcRate) {
		rs.feedStereoBlock(pcm, frames, srcRate);
	};
	if (!dec.init()) {
		setStatus(State::Error, "AAC init failed");
		return;
	}

	setStatus(State::Connecting, "Buffering\xe2\x80\xa6");
	bool playing = false;
	bool haveLast = false;
	uint64_t lastSeq = 0;

	while (running.load(std::memory_order_acquire) && !abort.load(std::memory_order_acquire) && !rs.stopped) {
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
			if (rs.stopped)
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
