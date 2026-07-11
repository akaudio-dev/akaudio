// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

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
	// May run on the audio device thread (see ratePending): store + flag only, never
	// lock. mixLoop recomputes the interval length + drops mismatched queued audio at
	// its next boundary. If mixLoop isn't running (pre-join), the flag simply waits;
	// join's setTempo does the first real compute with this rate anyway.
	sampleRate.store(sr, std::memory_order_relaxed);
	ratePending.store(true, std::memory_order_release);
}

void NjAudio::recomputeIntervalLocked() {
	double sr = sampleRate.load(std::memory_order_relaxed);
	int n = 0;
	if (bpm > 0 && bpi > 0 && sr > 0)
		n = (int)std::llround((double)bpi * 60.0 * sr / (double)bpm);
	// Sanity cap (~87 s at 48 kHz): a broken/hostile server tempo (e.g. bpm=1)
	// must not size the per-interval decode buffers into the gigabytes.
	if (n > (1 << 22))
		n = 0;
	int prev = intervalSamples.exchange(n, std::memory_order_relaxed);
	if (n != prev) {
		// Interval length changed (tempo/sr): queued intervals were gridded to the old
		// length; drop them so we don't mix mismatched buffers.
		for (auto& kv : channels)
			kv.second.ready.clear();
	}
}

void NjAudio::recomputeInterval() {
	std::lock_guard<std::mutex> lock(mu);
	recomputeIntervalLocked();
}

void NjAudio::setTempo(int newBpm, int newBpi) {
	std::lock_guard<std::mutex> lock(mu);
	if (intervalSamples.load(std::memory_order_relaxed) <= 0) {
		// No interval established yet (the initial config on join): apply immediately so
		// the mixer/tx can start. Otherwise defer to the next interval boundary, applied
		// in mixLoop — like njclient/JamTaba, a config change takes effect at the next
		// interval cycle, leaving the current interval's audio untouched.
		bpm = newBpm;
		bpi = newBpi;
		recomputeIntervalLocked();
	} else {
		pendingBpm = newBpm;
		pendingBpi = newBpi;
		tempoPending = true;
	}
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
	ch.user = user;
	ch.active = active;
	ch.gainL = gl;
	ch.gainR = gr;
}

int NjAudio::assignSlot(const std::string& user) {
	auto it = userSlot.find(user);
	if (it != userSlot.end())
		return it->second;
	for (int i = 0; i < MAX_PLAYERS; i++) {
		if (!slotUsed[i]) {
			slotUsed[i] = true;
			userSlot[user] = i;
			return i;
		}
	}
	return -1; // more than MAX_PLAYERS users -> this one is off the poly bundle
}

void NjAudio::refreshSlots() {
	// Free slots whose user no longer has any channel; recompute the poly channel count.
	for (auto it = userSlot.begin(); it != userSlot.end();) {
		bool present = false;
		for (auto& kv : channels)
			if (kv.second.user == it->first) { present = true; break; }
		if (!present) {
			slotUsed[it->second] = false;
			it = userSlot.erase(it);
		} else {
			++it;
		}
	}
	int hi = 0;
	for (int i = 0; i < MAX_PLAYERS; i++)
		if (slotUsed[i]) hi = i + 1;
	nPoly.store(hi, std::memory_order_relaxed);
}

void NjAudio::beginInterval(const std::string& user, int chidx, const uint8_t guid[16], uint32_t fourcc,
		uint32_t estSize) {
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
	// The protocol allows one in-flight transfer per channel. If a previous interval on
	// this channel never got its last-flagged WRITE (remote user dropped mid-interval),
	// its entry would leak forever — drop any pending transfer for the same channel so
	// the map can't accumulate abandoned buffers.
	for (auto it = transfers.begin(); it != transfers.end(); ) {
		if (it->second.chanKey == key) it = transfers.erase(it);
		else ++it;
	}
	Transfer t;
	t.chanKey = key;
	t.ogg = ogg;
	// estSize is the server's advertised interval size; reserving up front avoids repeated
	// reallocs as DOWNLOAD_WRITE chunks stream in. Cap it — it's an unvalidated wire value.
	if (estSize > 0)
		t.bytes.reserve(std::min<uint32_t>(estSize, 4u << 20));
	transfers[std::string((const char*)guid, 16)] = std::move(t);
}

void NjAudio::writeInterval(const uint8_t guid[16], const uint8_t* data, size_t len, bool last) {
	auto it = transfers.find(std::string((const char*)guid, 16));
	if (it == transfers.end())
		return; // unknown/zero-guid transfer
	Transfer& t = it->second;
	// Bound the accumulated interval: a legit OGG interval is a few MB (worst ~5 MB at
	// the ~87 s cap), so a stream that keeps sending past this is broken/hostile —
	// abandon it as a silence interval rather than growing t.bytes unboundedly.
	if (data && len && t.bytes.size() + len <= (16u << 20))
		t.bytes.insert(t.bytes.end(), data, data + len);
	else if (data && len) {
		enqueue(t.chanKey, std::vector<float>()); // keep cadence
		transfers.erase(it);
		return;
	}
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
	if (ch.user.empty()) // interval may arrive before USERINFO; derive user from the key
		ch.user = key.substr(0, key.rfind('\n'));
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
	int srcRate = (int)info.sample_rate;
	// channels and sample_rate come straight off the wire (unvalidated in the OGG
	// header). Reject junk before it drives buffer sizing below — a lying rate would
	// otherwise size a multi-hundred-GB allocation (bad_alloc -> host crash).
	if (nch < 1 || nch > 2 || srcRate < 8000 || srcRate > 192000) {
		stb_vorbis_close(v);
		return {};
	}

	// Decode the whole interval to interleaved float at the source rate, straight into
	// `in` (no per-chunk bounce buffer): pre-size from the expected source length —
	// known up front from the interval grid and the rate ratio — and grow in large
	// steps only if the estimate falls short.
	double engineRate = sampleRate.load(std::memory_order_relaxed);
	const size_t CHUNK = 4096;
	size_t expectFrames = (engineRate > 0)
		? (size_t)((double)frames * srcRate / engineRate)
		: (size_t)frames;
	std::vector<float> in((expectFrames + CHUNK) * nch);
	size_t used = 0; // floats written so far
	int got;
	for (;;) {
		// Guarantee at least CHUNK*nch floats of headroom before each decode call —
		// stb_vorbis writes up to that many and doesn't know the remaining capacity,
		// so a single +50% grow (which only restores CHUNK*nch when the buffer is
		// already large) is not enough; take the max with the exact requirement.
		if (in.size() - used < CHUNK * nch)
			in.resize(std::max(in.size() + in.size() / 2, used + CHUNK * nch));
		got = stb_vorbis_get_samples_float_interleaved(v, nch, in.data() + used, (int)(CHUNK * nch));
		if (got <= 0)
			break;
		used += (size_t)got * nch;
	}
	stb_vorbis_close(v);

	int srcFrames = (int)(used / nch);
	if (srcFrames <= 0)
		return {};

	// Resample by the FIXED source-rate -> engine-rate ratio and downmix to stereo, so
	// playback speed depends only on sample rates — never on BPM/BPI (matches njclient/
	// JamTaba). The interval grid is a SEPARATE concern: size the output to `frames`,
	// padding with silence if the recording is shorter or truncating if longer (e.g. a
	// pre-change interval landing after a BPI change). This decouples pitch from the grid,
	// so a BPI change no longer doubles/halves the tempo.
	double ratio = (srcRate > 0 && engineRate > 0)
		? (double)srcRate / engineRate          // samples to advance per output sample
		: (double)srcFrames / (double)frames;   // fallback if rates unknown (old behaviour)
	int outFrames = (int)std::llround((double)srcFrames / ratio); // srcFrames * engineRate/srcRate
	if (outFrames < 0)
		outFrames = 0;

	std::vector<float> out((size_t)frames * 2, 0.f); // sized to the interval grid (silence-padded)
	int limit = std::min(outFrames, frames);
	auto srcStereo = [&](int idx, float& l, float& r) {
		if (idx < 0) idx = 0;
		if (idx >= srcFrames) idx = srcFrames - 1;
		const float* f = &in[(size_t)idx * nch];
		l = f[0];
		r = nch >= 2 ? f[1] : f[0];
	};
	for (int i = 0; i < limit; i++) {
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

void NjAudio::setTransmit(int channels, float quality) {
	if (channels > MAX_TX) channels = MAX_TX;
	// Atomics only — the capture rings are built once in start() (see below), never here,
	// so this can't race txLoop iterating the vector. captureFrame gates on txRings, so
	// setting txActive before the rings exist (setTransmit called pre-join) is harmless.
	txQuality.store(quality, std::memory_order_relaxed);
	nTx.store(channels, std::memory_order_release);
	txActive.store(channels > 0, std::memory_order_release);
}

void NjAudio::start() {
	if (running.exchange(true, std::memory_order_acq_rel))
		return; // already running
	abort.store(false, std::memory_order_release);
	// Build the capture rings ONCE, here, before spawning txThread — so the only
	// concurrent reader (txLoop) and the audio-thread producer (captureFrame, gated on
	// txRings) never see the vector being mutated. ~21 s @ 48 kHz headroom each.
	if (txRings.load(std::memory_order_relaxed) < MAX_TX) {
		txCapture.clear();
		for (int i = 0; i < MAX_TX; i++)
			txCapture.push_back(std::unique_ptr<StereoRingBuffer>(new StereoRingBuffer(1 << 20)));
		txRings.store(MAX_TX, std::memory_order_release);
	}
	mixThread = std::thread(&NjAudio::mixLoop, this);
	txThread = std::thread(&NjAudio::txLoop, this);
}

void NjAudio::stop() {
	abort.store(true, std::memory_order_release);
	if (mixThread.joinable())
		mixThread.join();
	if (txThread.joinable())
		txThread.join();
	running.store(false, std::memory_order_release);
	std::lock_guard<std::mutex> lock(mu);
	channels.clear();
	transfers.clear();
	userSlot.clear();
	for (int i = 0; i < MAX_PLAYERS; i++) slotUsed[i] = false;
	nPoly.store(0, std::memory_order_relaxed);
	// The audio thread (this ring's consumer) may still be pulling; it applies
	// the drop on its next pull. The tx capture rings are drained by txLoop
	// (their consumer) — while inactive and at thread start — never from here,
	// because the audio thread is their *producer* and may push concurrently.
	ring.requestClear();
}

// Transmit: pull complete intervals from each channel's capture ring, encode, hand up.
void NjAudio::txLoop() {
	int serial = 1;
	std::vector<float> interleaved;
	// This thread is the capture rings' consumer, so it owns discarding stale
	// audio: drain at start (a previous session's tail) and while TX is off
	// (so re-enabling starts a fresh, downbeat-aligned interval, instead of
	// uploading a stale partial one).
	auto drainCapture = [this]() {
		for (auto& c : txCapture)
			if (c) c->drain();
	};
	drainCapture();
	while (!abort.load(std::memory_order_acquire)) {
		int N = intervalSamples.load(std::memory_order_relaxed);
		int n = nTx.load(std::memory_order_relaxed);
		if (!txActive.load(std::memory_order_acquire)) {
			drainCapture();
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}
		if (N <= 0 || n <= 0 || N > (1 << 20)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}
		double sr = sampleRate.load(std::memory_order_relaxed);
		bool didAny = false;
		for (int ch = 0; ch < n && ch < (int) txCapture.size(); ch++) {
			if (!txCapture[ch] || txCapture[ch]->size() < (size_t) N)
				continue; // not a full interval captured yet
			interleaved.resize((size_t) N * 2);
			for (int i = 0; i < N; i++) {
				float l = 0.f, r = 0.f;
				txCapture[ch]->pull(l, r);
				interleaved[(size_t) i * 2] = l;
				interleaved[(size_t) i * 2 + 1] = r;
			}
			std::vector<uint8_t> ogg = encodeOggInterval(interleaved.data(), N, 2, (int) sr,
				txQuality.load(std::memory_order_relaxed), serial++);
			if (!ogg.empty() && onUploadInterval)
				onUploadInterval(ch, ogg);
			didAny = true;
		}
		if (!didAny)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

void NjAudio::mixLoop() {
	struct Src {
		std::vector<float> buf; // empty = silence
		float gl, gr;
		int slot; // poly channel, or -1 (overflow / off-bundle)
	};
	bool started = false;
	bool sawReady = false;
	std::chrono::steady_clock::time_point firstReady;

	while (!abort.load(std::memory_order_acquire)) {
		// Apply a deferred server tempo change here — the top of a cycle is an interval
		// boundary — never mid-interval. The grid length changed, so re-prebuffer.
		{
			std::lock_guard<std::mutex> lock(mu);
			if (tempoPending) {
				bpm = pendingBpm;
				bpi = pendingBpi;
				tempoPending = false;
				recomputeIntervalLocked();
				started = false;
				sawReady = false;
			}
			// Deferred sample-rate change (may have been posted from the audio device
			// thread): recompute the grid + drop mismatched queued audio here, off that
			// thread. recomputeIntervalLocked reads the already-stored sampleRate.
			if (ratePending.exchange(false, std::memory_order_acquire)) {
				recomputeIntervalLocked();
				started = false;
				sawReady = false;
			}
		}
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
				s.slot = assignSlot(ch.user); // stable poly channel for this user
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
			refreshSlots(); // free departed users' slots; update poly channel count
		}
		nActive.store(active, std::memory_order_relaxed);

		// Mix this interval into per-slot stereo wide frames; ring backpressure paces realtime.
		float frame[RING_CH];
		for (int i = 0; i < N; i++) {
			std::memset(frame, 0, sizeof(frame));
			for (const Src& s : srcs) {
				if (s.buf.empty() || s.slot < 0)
					continue;
				size_t idx = (size_t)i * 2;
				if (idx + 1 >= s.buf.size())
					continue; // past this interval's decoded audio -> silence (length guard)
				frame[s.slot * 2] += s.buf[idx] * s.gl;
				frame[s.slot * 2 + 1] += s.buf[idx + 1] * s.gr;
			}
			while (!ring.push(frame)) {
				if (abort.load(std::memory_order_acquire))
					return;
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
		}
	}
}

} // namespace nj
} // namespace akaudio
