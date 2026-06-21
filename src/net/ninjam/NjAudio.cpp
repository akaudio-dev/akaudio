#include "NjAudio.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "../../dep/stb_vorbis_impl.hpp"

namespace akaudio {
namespace nj {

NjAudio::~NjAudio() {
	stop();
}

std::string NjAudio::chanKey(const std::string& user, int chidx) {
	return user + "\n" + std::to_string(chidx);
}

void NjAudio::setSampleRate(double sr) {
	sampleRate.store(sr, std::memory_order_relaxed);
	recomputeInterval();
}

void NjAudio::recomputeInterval() {
	std::lock_guard<std::mutex> lock(mu);
	double sr = sampleRate.load(std::memory_order_relaxed);
	int n = 0;
	if (bpm > 0 && bpi > 0 && sr > 0)
		n = (int)std::llround((double)bpi * 60.0 * sr / (double)bpm);
	int prev = intervalSamples.exchange(n, std::memory_order_relaxed);
	if (n != prev) {
		// Interval length changed (tempo/sr): queued intervals no longer match; drop them.
		for (auto& kv : channels)
			kv.second.ready.clear();
	}
}

void NjAudio::setTempo(int newBpm, int newBpi) {
	{
		std::lock_guard<std::mutex> lock(mu);
		bpm = newBpm;
		bpi = newBpi;
	}
	recomputeInterval();
}

void NjAudio::onUserChannel(const std::string& user, int chidx, bool active, int volDb10, int pan) {
	// volume: dB*10 -> linear; pan: -128..127 -> -1..1 (linear pan law).
	float gain = std::pow(10.f, (float)volDb10 / 200.f);
	float np = (float)pan / 128.f;
	if (np < -1.f) np = -1.f;
	if (np > 1.f) np = 1.f;
	float gl = gain * (np > 0.f ? 1.f - np : 1.f);
	float gr = gain * (np < 0.f ? 1.f + np : 1.f);

	std::lock_guard<std::mutex> lock(mu);
	Channel& ch = channels[chanKey(user, chidx)];
	ch.active = active;
	ch.gainL = gl;
	ch.gainR = gr;
}

void NjAudio::beginInterval(const std::string& user, int chidx, const uint8_t guid[16], uint32_t fourcc) {
	static const uint8_t zero[16] = {0};
	std::string key = chanKey(user, chidx);
	if (std::memcmp(guid, zero, 16) == 0) {
		// Silence interval: enqueue an empty slot to keep this channel's cadence.
		enqueue(key, std::vector<float>());
		return;
	}
	// fourcc 'OGGv' (and 'OGG' family) = OGG Vorbis. Anything else (e.g. Opus) is
	// unsupported in v1 -> we just won't decode it (channel stays silent that interval).
	bool ogg = ((fourcc & 0xffffff) == ('O' | ('G' << 8) | ('G' << 16)));
	Transfer t;
	t.chanKey = key;
	t.fourcc = fourcc;
	t.ogg = ogg;
	transfers[std::string((const char*)guid, 16)] = std::move(t);
}

void NjAudio::writeInterval(const uint8_t guid[16], const uint8_t* data, size_t len, bool last) {
	auto it = transfers.find(std::string((const char*)guid, 16));
	if (it == transfers.end())
		return; // unknown/zero-guid transfer
	Transfer& t = it->second;
	if (data && len)
		t.bytes.insert(t.bytes.end(), data, data + len);
	if (!last)
		return;

	int frames = intervalSamples.load(std::memory_order_relaxed);
	std::vector<float> pcm;
	if (t.ogg && frames > 0 && !t.bytes.empty()) {
		pcm = decodeOgg(t.bytes.data(), t.bytes.size(), frames);
		if (pcm.empty())
			nErrors.fetch_add(1, std::memory_order_relaxed);
		else
			nDecoded.fetch_add(1, std::memory_order_relaxed);
	}
	// On failure / unsupported codec, pcm stays empty => a silence interval (keeps cadence).
	enqueue(t.chanKey, std::move(pcm));
	transfers.erase(it);
}

void NjAudio::enqueue(const std::string& key, std::vector<float>&& interval) {
	std::lock_guard<std::mutex> lock(mu);
	Channel& ch = channels[key];
	if (ch.ready.size() >= kMaxReady)
		ch.ready.pop_front(); // mixer fell behind; drop oldest to bound latency/memory
	ch.ready.push_back(std::move(interval));
}

std::vector<float> NjAudio::decodeOgg(const uint8_t* data, size_t len, int frames) {
	int err = 0;
	stb_vorbis* v = stb_vorbis_open_memory(data, (int)len, &err, nullptr);
	if (!v)
		return {};
	stb_vorbis_info info = stb_vorbis_get_info(v);
	int nch = info.channels;
	if (nch < 1) { stb_vorbis_close(v); return {}; }

	// Decode the whole interval to interleaved float at the source rate.
	std::vector<float> in;
	const int CHUNK = 4096;
	std::vector<float> tmp((size_t)CHUNK * nch);
	int got;
	while ((got = stb_vorbis_get_samples_float_interleaved(v, nch, tmp.data(), CHUNK * nch)) > 0)
		in.insert(in.end(), tmp.begin(), tmp.begin() + (size_t)got * nch);
	stb_vorbis_close(v);

	int srcFrames = (int)(in.size() / nch);
	if (srcFrames <= 0)
		return {};

	// Resample srcFrames -> `frames` (this also converts source rate -> engine rate,
	// since every client's interval spans the same musical length) and downmix to stereo.
	std::vector<float> out((size_t)frames * 2);
	auto srcStereo = [&](int idx, float& l, float& r) {
		if (idx < 0) idx = 0;
		if (idx >= srcFrames) idx = srcFrames - 1;
		const float* f = &in[(size_t)idx * nch];
		l = f[0];
		r = nch >= 2 ? f[1] : f[0];
	};
	double ratio = (double)srcFrames / (double)frames;
	for (int i = 0; i < frames; i++) {
		double pos = (double)i * ratio;
		int i0 = (int)pos;
		double frac = pos - i0;
		float l0, r0, l1, r1;
		srcStereo(i0, l0, r0);
		srcStereo(i0 + 1, l1, r1);
		out[(size_t)i * 2] = l0 + (l1 - l0) * (float)frac;
		out[(size_t)i * 2 + 1] = r0 + (r1 - r0) * (float)frac;
	}
	return out;
}

void NjAudio::start() {
	if (running.exchange(true, std::memory_order_acq_rel))
		return; // already running
	abort.store(false, std::memory_order_release);
	mixThread = std::thread(&NjAudio::mixLoop, this);
}

void NjAudio::stop() {
	abort.store(true, std::memory_order_release);
	if (mixThread.joinable())
		mixThread.join();
	running.store(false, std::memory_order_release);
	std::lock_guard<std::mutex> lock(mu);
	channels.clear();
	transfers.clear();
	ring.clear();
}

void NjAudio::mixLoop() {
	struct Src {
		std::vector<float> buf; // empty = silence
		float gl, gr;
	};
	bool started = false;
	bool sawReady = false;
	std::chrono::steady_clock::time_point firstReady;

	while (!abort.load(std::memory_order_acquire)) {
		int N = intervalSamples.load(std::memory_order_relaxed);
		if (N <= 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		// Prebuffer before starting the cadence. We don't begin the instant the first
		// interval decodes — that leaves zero timing margin, so every interval's
		// decode/network jitter races the play boundary and an occasional miss plays a
		// full interval of silence. Holding a small margin (a fraction of an interval,
		// capped) shifts every boundary that much later than decode-completion, so the
		// next interval is always ready in time. Costs that much extra startup lead-in,
		// not a whole interval. (Mirrors the canonical client's config_play_prebuffer.)
		if (!started) {
			bool any = false;
			{
				std::lock_guard<std::mutex> lock(mu);
				for (auto& kv : channels)
					for (auto& iv : kv.second.ready)
						if (!iv.empty()) { any = true; break; }
			}
			if (!any) {
				sawReady = false; // reset margin timer until audio actually shows up
				std::this_thread::sleep_for(std::chrono::milliseconds(15));
				continue;
			}
			auto now = std::chrono::steady_clock::now();
			if (!sawReady) { sawReady = true; firstReady = now; }
			double sr = sampleRate.load(std::memory_order_relaxed);
			double intervalSec = sr > 0 ? (double) N / sr : 0.0;
			double margin = std::min(2.0, intervalSec * 0.4); // play-prebuffer headroom
			if (std::chrono::duration<double>(now - firstReady).count() < margin) {
				std::this_thread::sleep_for(std::chrono::milliseconds(15));
				continue;
			}
			started = true;
		}

		// Interval boundary: take one ready interval from each participating channel.
		std::vector<Src> srcs;
		int active = 0;
		{
			std::lock_guard<std::mutex> lock(mu);
			for (auto it = channels.begin(); it != channels.end();) {
				Channel& ch = it->second;
				if (!ch.active && ch.ready.empty()) {
					it = channels.erase(it); // gone and drained
					continue;
				}
				if (ch.active) active++;
				Src s;
				s.gl = ch.gainL;
				s.gr = ch.gainR;
				if (!ch.ready.empty()) {
					s.buf = std::move(ch.ready.front());
					ch.ready.pop_front();
				} else if (ch.active) {
					// Active channel but nothing decoded in time -> a one-interval silence.
					nMissed.fetch_add(1, std::memory_order_relaxed);
				}
				srcs.push_back(std::move(s));
				++it;
			}
		}
		nActive.store(active, std::memory_order_relaxed);

		// Mix this interval and push frame-by-frame; ring backpressure paces to realtime.
		for (int i = 0; i < N; i++) {
			float l = 0.f, r = 0.f;
			for (const Src& s : srcs) {
				if (s.buf.empty())
					continue;
				l += s.buf[(size_t)i * 2] * s.gl;
				r += s.buf[(size_t)i * 2 + 1] * s.gr;
			}
			while (!ring.push(l, r)) {
				if (abort.load(std::memory_order_acquire))
					return;
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
		}
	}
}

} // namespace nj
} // namespace akaudio
