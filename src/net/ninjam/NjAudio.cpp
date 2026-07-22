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

void NjAudio::onUserChannel(const std::string& user, int chidx, bool active, int volDb10, int pan,
                            uint8_t flags) {
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
	bool voice = (flags & 2) != 0;
	if (voice != ch.voice) {
		// Mode flip: queued audio belongs to the other playback discipline — drop it.
		ch.voice = voice;
		ch.ready.clear();
		ch.cur.clear();
		ch.curPos = 0;
		ch.silenceLeft = 0;
		ch.holdFrames = -1;
		ch.playing = false;
		ch.vfifo.clear();
		ch.vhead = 0;
		ch.vstarted = false;
		ch.everStarted = false; // interval discipline starts fresh (preview re-opens)
		ch.pfade = 0;
	}
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
	// Voice channels have no interval cadence — they play live from a FIFO. A channel
	// whose chain hasn't locked yet gets the join-gap preview (progressive decode of
	// this very interval while it streams in).
	bool voice, started;
	{
		std::lock_guard<std::mutex> lock(mu);
		auto it = channels.find(key);
		voice = (it != channels.end()) && it->second.voice;
		started = (it != channels.end()) && it->second.everStarted;
	}
	if (std::memcmp(guid, zero, 16) == 0) {
		// Silence interval: enqueue an empty slot to keep this channel's cadence.
		// (Voice channels just fall silent — no cadence to keep.)
		if (!voice)
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
		if (it->second.chanKey == key) { closeTransfer(it->second); it = transfers.erase(it); }
		else ++it;
	}
	Transfer t;
	t.chanKey = key;
	t.ogg = ogg;
	t.voice = voice;
	t.preview = ogg && !voice && !started;
	// estSize is the server's advertised interval size; reserving up front avoids repeated
	// reallocs as DOWNLOAD_WRITE chunks stream in. Cap it — it's an unvalidated wire value.
	// (Streaming live uploads advertise 0; voice transfers keep only an undecoded tail.)
	if (!voice && estSize > 0)
		t.bytes.reserve(std::min<uint32_t>(estSize, 4u << 20));
	transfers[std::string((const char*)guid, 16)] = std::move(t);
}

void NjAudio::writeInterval(const uint8_t guid[16], const uint8_t* data, size_t len, bool last) {
	auto it = transfers.find(std::string((const char*)guid, 16));
	if (it == transfers.end())
		return; // unknown/zero-guid transfer
	Transfer& t = it->second;
	if (t.voice) {
		// Voice: decode as the bytes arrive and hand frames straight to the channel's
		// live FIFO — no interval assembly, no boundary wait.
		if (t.ogg && data && len)
			pushdataFeed(t, t.bytes, data, len);
		if (last) {
			closeTransfer(t);
			transfers.erase(it);
		}
		return;
	}
	// Join-gap preview: progressively decode this interval into the channel's live
	// FIFO too (t.bytes still accumulates whole for the proper decode at `last`). The
	// moment the channel's chain locks, drop the preview machinery — the fade-out in
	// mixLoop consumes what's already delivered.
	if (t.preview && data && len) {
		bool started;
		{
			std::lock_guard<std::mutex> lock(mu);
			auto c = channels.find(t.chanKey);
			started = (c != channels.end()) && c->second.everStarted;
		}
		if (started) {
			t.preview = false;
			closeTransfer(t);
			t.ptail.clear();
			t.pcm.clear();
		} else {
			pushdataFeed(t, t.ptail, data, len);
		}
	}
	// Bound the accumulated interval: a legit OGG interval is a few MB (worst ~5 MB at
	// the ~87 s cap), so a stream that keeps sending past this is broken/hostile —
	// abandon it as a silence interval rather than growing t.bytes unboundedly.
	if (data && len && t.bytes.size() + len <= (16u << 20))
		t.bytes.insert(t.bytes.end(), data, data + len);
	else if (data && len) {
		enqueue(t.chanKey, std::vector<float>()); // keep cadence
		closeTransfer(t); // a preview transfer holds a pushdata decoder
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
	closeTransfer(t); // a preview transfer holds a pushdata decoder
	transfers.erase(it);
}

void NjAudio::closeTransfer(Transfer& t) {
	if (t.pv) {
		stb_vorbis_close(t.pv);
		t.pv = nullptr;
	}
}

// Net thread: push arriving OGG bytes through a stb_vorbis pushdata decoder into the
// channel's live FIFO. `tail` holds the undecoded remainder between calls — the voice
// path consumes t.bytes in place; the preview path uses t.ptail so t.bytes stays whole.
void NjAudio::pushdataFeed(Transfer& t, std::vector<uint8_t>& tail, const uint8_t* data, size_t len) {
	// Bound the undecoded tail; a healthy stream stays tiny because we consume as fast
	// as chunks arrive. On overflow drop it and let the decoder resync at a page.
	if (tail.size() + len > (1u << 20)) {
		tail.clear();
		if (t.pv)
			stb_vorbis_flush_pushdata(t.pv);
	}
	tail.insert(tail.end(), data, data + len);
	size_t off = 0;
	if (!t.pv) {
		int used = 0, err = 0;
		t.pv = stb_vorbis_open_pushdata(tail.data(), (int)tail.size(), &used, &err, nullptr);
		if (!t.pv)
			return; // headers incomplete yet (or junk — then it simply never opens)
		off = (size_t)used;
		stb_vorbis_info info = stb_vorbis_get_info(t.pv);
		t.pvChannels = info.channels;
		t.pvRate = (int)info.sample_rate;
		// Wire values — reject junk before it drives buffer sizing.
		if (t.pvChannels < 1 || t.pvChannels > 2 || t.pvRate < 8000 || t.pvRate > 192000) {
			closeTransfer(t);
			tail.clear();
			nErrors.fetch_add(1, std::memory_order_relaxed);
			return;
		}
	}
	for (;;) {
		float** outputs = nullptr;
		int nch = 0, samples = 0;
		int used = stb_vorbis_decode_frame_pushdata(t.pv, tail.data() + off,
			(int)(tail.size() - off), &nch, &outputs, &samples);
		if (used == 0)
			break; // needs more data
		off += (size_t)used;
		if (samples > 0 && nch >= 1) {
			// Downmix/duplicate to stereo into the resample staging buffer.
			size_t base = t.pcm.size();
			t.pcm.resize(base + (size_t)samples * 2);
			const float* L = outputs[0];
			const float* R = nch >= 2 ? outputs[1] : outputs[0];
			for (int i = 0; i < samples; i++) {
				t.pcm[base + (size_t)i * 2] = L[i];
				t.pcm[base + (size_t)i * 2 + 1] = R[i];
			}
		}
	}
	if (off > 0)
		tail.erase(tail.begin(), tail.begin() + off);
	voiceDeliver(t);
}

// Net thread: fixed-ratio linear resample of staged frames -> channel FIFO (voice
// channels and join-gap previews share this delivery).
void NjAudio::voiceDeliver(Transfer& t) {
	double engineRate = sampleRate.load(std::memory_order_relaxed);
	if (t.pvRate <= 0 || engineRate <= 0)
		return;
	double ratio = (double)t.pvRate / engineRate; // source frames per output frame
	int srcFrames = (int)(t.pcm.size() / 2);
	std::vector<float> out;
	double pos = t.rsPos;
	while (pos + 1.0 < (double)srcFrames) { // interpolation needs pos+1
		int i0 = (int)pos;
		double frac = pos - i0;
		const float* a = &t.pcm[(size_t)i0 * 2];
		const float* b = &t.pcm[(size_t)(i0 + 1) * 2];
		out.push_back(a[0] + (float)frac * (b[0] - a[0]));
		out.push_back(a[1] + (float)frac * (b[1] - a[1]));
		pos += ratio;
	}
	// Retire fully-consumed source frames, keep the fractional phase.
	int consumed = (int)pos;
	if (consumed > 0) {
		if (consumed > srcFrames) consumed = srcFrames;
		t.pcm.erase(t.pcm.begin(), t.pcm.begin() + (size_t)consumed * 2);
		t.rsPos = pos - consumed;
	}
	if (out.empty())
		return;
	std::lock_guard<std::mutex> lock(mu);
	auto it = channels.find(t.chanKey);
	if (it == channels.end())
		return;
	Channel& ch = it->second;
	// Preview delivery ends the moment the chain locks — the fade-out only consumes
	// what's already queued; anything later belongs to the (replayed) chained slot.
	if (!ch.voice && ch.everStarted)
		return;
	ch.vfifo.insert(ch.vfifo.end(), out.begin(), out.end());
	// Skip-ahead: cap the backlog so a network stall never turns into permanently added
	// latency. Voice mirrors njclient's live mode (~0.75 s); a preview rides ~a beat or
	// two behind the sender and only lives for one interval, so give it more slack —
	// each skip is an audible phase jump.
	size_t capF = (size_t)(engineRate * (ch.voice ? 0.75 : 4.0));
	size_t haveF = (ch.vfifo.size() - ch.vhead) / 2;
	if (haveF > capF)
		ch.vhead += (haveF - capF) * 2;
	// Compact once the consumed prefix dominates.
	if (ch.vhead > (1u << 16) && ch.vhead > ch.vfifo.size() / 2) {
		ch.vfifo.erase(ch.vfifo.begin(), ch.vfifo.begin() + ch.vhead);
		ch.vhead = 0;
	}
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

void NjAudio::setTransmit(int channels, float quality, bool voice) {
	if (channels > MAX_TX) channels = MAX_TX;
	// Atomics only — the capture rings are built once in start() (see below), never here,
	// so this can't race txLoop iterating the vector. captureFrame gates on txRings, so
	// setting txActive before the rings exist (setTransmit called pre-join) is harmless.
	txQuality.store(quality, std::memory_order_relaxed);
	txVoice.store(voice, std::memory_order_relaxed);
	nTx.store(channels, std::memory_order_release);
	txActive.store(channels > 0, std::memory_order_release);
}

void NjAudio::start() {
	if (running.exchange(true, std::memory_order_acq_rel))
		return; // already running
	abort.store(false, std::memory_order_release);
	audioStarted_.store(false, std::memory_order_relaxed); // fresh join gap begins
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
	// Clear now (not only in start()) so the UI's join-gap countdown isn't suppressed
	// by a previous session's flag between a re-join and its mixer launch.
	audioStarted_.store(false, std::memory_order_relaxed);
	std::lock_guard<std::mutex> lock(mu);
	channels.clear();
	for (auto& kv : transfers)
		closeTransfer(kv.second); // free any voice pushdata decoders
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

// Transmit: stream each channel's captured frames through a per-interval OGG encoder,
// handing encoded chunks up AS THE INTERVAL IS CAPTURED (like the canonical njclient):
// BEGIN at interval start, WRITE chunks while playing, the final flagged chunk right at
// the interval boundary. Receivers can then play the interval at their very next
// boundary — a whole interval earlier than the old capture-everything-then-upload
// scheme. Feeding from the ring incrementally also means an interval LONGER than the
// ring (e.g. 32 BPI at 80 BPM = 24 s = 1,152,000 samples @48k > the 1<<20-frame ring)
// encodes fine: the ring only ever holds the short gap between drains.
void NjAudio::txLoop() {
	int serial = 1;
	// Per-channel in-flight interval state.
	struct TxCh {
		OggIntervalEncoder enc;
		int framesDone = 0; // frames fed to enc this interval
		bool open = false;  // BEGIN announced, WRITE stream in progress
	};
	std::vector<TxCh> tx(txCapture.size());
	int gridN = 0;                  // interval length the open intervals are gridded to
	int pendingPrefill = 0;         // silence frames to lead the next-started intervals with
	std::vector<float> chunk;       // pull/feed bounce buffer
	const int CHUNKF = 4096;        // frames per feed

	// This thread is the capture rings' consumer, so it owns discarding stale audio:
	// drain at start (a previous session's tail) and while TX is off, so re-enabling
	// starts a fresh, beat-aligned interval instead of uploading a stale partial one.
	auto drainCapture = [this]() {
		for (auto& c : txCapture)
			if (c) c->drain();
	};
	// Close an in-flight interval: flush the encoder and send the final flagged chunk
	// (an interval shorter than the grid is legal — receivers pad with silence).
	auto closeInterval = [&](int ch) {
		if (!tx[ch].open)
			return;
		tx[ch].enc.finish();
		std::vector<uint8_t> tail = tx[ch].enc.take();
		if (onUploadData)
			onUploadData(ch, tail.empty() ? nullptr : tail.data(), tail.size(), /*last=*/true);
		tx[ch].open = false;
		tx[ch].framesDone = 0;
	};
	auto closeAll = [&]() {
		for (size_t ch = 0; ch < tx.size(); ch++)
			closeInterval((int) ch);
	};
	// Announce + start a new interval on `ch`, leading with `prefill` silence frames.
	auto startInterval = [&](int ch, int N, double sr, int prefill) {
		if (!tx[ch].enc.begin(2, (int) sr, txQuality.load(std::memory_order_relaxed), serial++))
			return; // encoder rejected params; stay closed (retried next pass)
		tx[ch].open = true;
		tx[ch].framesDone = 0;
		if (onUploadBegin)
			onUploadBegin(ch);
		if (prefill > N) prefill = N;
		if (prefill > 0) {
			chunk.assign((size_t) CHUNKF * 2, 0.f);
			int left = prefill;
			while (left > 0) {
				int m = left > CHUNKF ? CHUNKF : left;
				tx[ch].enc.feed(chunk.data(), m);
				left -= m;
			}
			tx[ch].framesDone = prefill;
		}
		std::vector<uint8_t> head = tx[ch].enc.take(); // headers (+ prefill pages)
		if (!head.empty() && onUploadData)
			onUploadData(ch, head.data(), head.size(), /*last=*/false);
	};

	drainCapture();
	while (!abort.load(std::memory_order_acquire)) {
		int N = intervalSamples.load(std::memory_order_relaxed);
		int n = nTx.load(std::memory_order_relaxed);
		double srNow = sampleRate.load(std::memory_order_relaxed);
		// Voice mode: rolling fixed-length intervals (canonical njclient's LL_CHUNK_SIZE
		// = 2 s), independent of the room tempo — no grid, no arming, minimal latency.
		if (txVoice.load(std::memory_order_relaxed) && srNow > 0)
			N = (int) (srNow * 2.0);
		if (!txActive.load(std::memory_order_acquire)) {
			closeAll();
			drainCapture();
			txArmPending.store(false, std::memory_order_relaxed);
			pendingPrefill = 0;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}
		// Same sanity ceiling as the decode side (recomputeIntervalLocked): reject only
		// a truly broken tempo (~87 s @48k), never a legitimate long interval.
		if (N <= 0 || n <= 0 || N > (1 << 22)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}
		if (N != gridN) {
			// Tempo / sample-rate re-grid: in-flight intervals are gridded to the old
			// length — close them short and start fresh (the module disarms + re-arms
			// its capture on a config change, so a new arming request follows).
			closeAll();
			drainCapture();
			gridN = N;
		}
		// A new arming request: capture (re)started at a beat boundary on the audio
		// thread. Close anything in flight and lead the next intervals with silence
		// for the elapsed part of the current room interval.
		if (txArmPending.exchange(false, std::memory_order_acq_rel)) {
			closeAll();
			drainCapture(); // pre-arm remnants (e.g. between disarm and re-arm)
			int num = txArmNum.load(std::memory_order_relaxed);
			int den = txArmDen.load(std::memory_order_relaxed);
			// Voice mode has no grid — an arming request only marks the capture start.
			pendingPrefill = (!txVoice.load(std::memory_order_relaxed) && den > 0 && num > 0)
				? (int) (((long long) N * num) / den) : 0;
		}

		double sr = sampleRate.load(std::memory_order_relaxed);
		bool didAny = false;
		for (int ch = 0; ch < n && ch < (int) txCapture.size(); ch++) {
			if (!txCapture[ch])
				continue;
			for (;;) {
				size_t avail = txCapture[ch]->size();
				if (!tx[ch].open) {
					if (avail == 0)
						break; // nothing captured yet — start lazily when audio arrives
					// (Seeing captured frames means any armTransmit that preceded them
					// is visible too — the ring's release/acquire pairing carries it —
					// so pendingPrefill is already set when we get here.)
					startInterval(ch, N, sr, pendingPrefill);
					if (!tx[ch].open)
						break;
					didAny = true;
				}
				int remain = N - tx[ch].framesDone;
				int m = (int) std::min<size_t>(std::min((size_t) remain, avail), (size_t) CHUNKF);
				if (m > 0) {
					chunk.resize((size_t) m * 2);
					for (int i = 0; i < m; i++) {
						float l = 0.f, r = 0.f;
						txCapture[ch]->pull(l, r);
						chunk[(size_t) i * 2] = l;
						chunk[(size_t) i * 2 + 1] = r;
					}
					tx[ch].enc.feed(chunk.data(), m);
					tx[ch].framesDone += m;
					std::vector<uint8_t> bytes = tx[ch].enc.take();
					if (!bytes.empty() && onUploadData)
						onUploadData(ch, bytes.data(), bytes.size(), /*last=*/false);
					didAny = true;
				}
				if (tx[ch].framesDone >= N) {
					// Interval boundary: close it out. The next interval starts lazily
					// with the next captured frame (continuous capture => immediately),
					// with no prefill — it begins exactly on the grid.
					closeInterval(ch);
					pendingPrefill = 0;
					continue;
				}
				if (m <= 0)
					break; // ring drained; wait for more capture
			}
		}
		if (!didAny)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	// Session over (leave/stop): close any in-flight interval so the server and
	// receivers aren't left with a dangling transfer (harmless either way — they
	// time out — but this is tidier and the socket may still be up on a clean stop).
	closeAll();
}

// Playout. ARRIVAL-LOCKED per-channel playheads instead of a global boundary cadence:
// a channel's first completed interval starts playing (after a short jitter hold) the
// moment it is available and later ones chain seamlessly every N frames — phase-true
// to the SENDER's grid. The old scheme (pop one interval per self-clocked boundary,
// like the canonical client's local-boundary rule) added an arbitrary 0..1 extra
// interval of latency depending on clock phase; with senders that stream their
// uploads, arrival time ≈ the sender's boundary, so locking to arrival pins latency
// at "one interval + network" for every pairing. Voice channels bypass intervals
// entirely: they mix live from their FIFO with a tiny prebuffer.
void NjAudio::mixLoop() {
	const int BLOCK = 256;                        // frames mixed per lock acquisition
	const size_t VOICE_PREBUF = 2048;             // ~43 ms @48k before a voice channel starts
	std::vector<float> block((size_t) BLOCK * RING_CH);

	while (!abort.load(std::memory_order_acquire)) {
		{
			std::lock_guard<std::mutex> lock(mu);
			// Deferred server tempo / sample-rate changes: recompute the grid. Queued
			// intervals are dropped by the recompute; also drop in-flight playheads —
			// they're gridded to the old length.
			bool regrid = false;
			if (tempoPending) {
				bpm = pendingBpm;
				bpi = pendingBpi;
				tempoPending = false;
				regrid = true;
			}
			if (ratePending.exchange(false, std::memory_order_acquire))
				regrid = true;
			if (regrid) {
				recomputeIntervalLocked();
				for (auto& kv : channels) {
					Channel& ch = kv.second;
					ch.cur.clear();
					ch.curPos = 0;
					ch.silenceLeft = 0;
					ch.holdFrames = -1;
					ch.playing = false;
					if (!ch.voice) {
						// Re-grid re-opens the preview window: the chain re-locks on the
						// next arrival (up to a whole interval away), so the first
						// interval at the new tempo streams live meanwhile — the same
						// treatment as the join gap.
						ch.everStarted = false;
						ch.vfifo.clear();
						ch.vhead = 0;
						ch.vstarted = false;
						ch.pfade = 0;
					}
				}
			}
		}
		int N = intervalSamples.load(std::memory_order_relaxed);
		double sr = sampleRate.load(std::memory_order_relaxed);
		// Startup jitter hold: streamed uploads complete right at the sender's boundary,
		// so the chain slack is only network jitter — hold the first interval briefly so
		// every later one arrives in time. (Replaces the old global prebuffer margin.)
		const int holdInit = (int) std::min(sr * 1.0, (double) N * 0.25);
		// Preview pacing: buffer ~0.5 s before starting (senders' OGG pages arrive in
		// ~0.2 s bursts — the voice prebuf is too tight for that), fade the retired
		// preview tail out over ~0.25 s when the chain takes over.
		const size_t previewPrebuf = (size_t) (sr * 0.5);
		const int pfadeTotal = std::max(1, (int) (sr * 0.25));

		std::memset(block.data(), 0, block.size() * sizeof(float));
		bool anything = false; // any channel present (else idle-sleep instead of pacing silence)
		int active = 0;
		{
			std::lock_guard<std::mutex> lock(mu);
			for (auto it = channels.begin(); it != channels.end();) {
				Channel& ch = it->second;
				if (!ch.active && ch.voice) {
					// A departed voice channel has no cadence to play out — drop the tail
					// (a sub-prebuffer remnant would otherwise pin the channel + slot).
					ch.vfifo.clear();
					ch.vhead = 0;
				}
				bool vEmpty = (ch.vfifo.size() - ch.vhead) == 0;
				bool drained = ch.ready.empty() && ch.cur.empty() && ch.silenceLeft == 0 && vEmpty;
				if (!ch.active && drained) {
					it = channels.erase(it); // gone and fully played out
					continue;
				}
				if (ch.active) active++;
				anything = true;
				int slot = assignSlot(ch.user);
				float* out = slot >= 0 ? &block[(size_t) slot * 2] : nullptr; // wide-frame stride below
				if (ch.voice) {
					// Live FIFO with a small prebuffer; running dry re-arms the prebuffer.
					size_t availF = (ch.vfifo.size() - ch.vhead) / 2;
					if (!ch.vstarted) {
						if (availF < VOICE_PREBUF) { ++it; continue; }
						ch.vstarted = true;
					}
					int m = (int) std::min<size_t>(availF, (size_t) BLOCK);
					for (int i = 0; i < m && out; i++) {
						out[(size_t) i * RING_CH]     += ch.vfifo[ch.vhead + (size_t) i * 2] * ch.gainL;
						out[(size_t) i * RING_CH + 1] += ch.vfifo[ch.vhead + (size_t) i * 2 + 1] * ch.gainR;
					}
					ch.vhead += (size_t) m * 2;
					if (m > 0)
						audioStarted_.store(true, std::memory_order_relaxed);
					if (m < BLOCK)
						ch.vstarted = false; // ran dry — rebuffer before resuming
				} else if (N > 0) {
					int i = 0;
					while (i < BLOCK) {
						if (ch.silenceLeft > 0) {
							// A silence interval: consume without mixing (keeps cadence).
							int m = std::min(BLOCK - i, ch.silenceLeft);
							ch.silenceLeft -= m;
							i += m;
							continue;
						}
						if (ch.cur.empty()) {
							if (ch.ready.empty()) {
								if (ch.playing) {
									// Chain broke (late interval): count it and re-lock
									// to arrival when the next one lands.
									nMissed.fetch_add(1, std::memory_order_relaxed);
									ch.playing = false;
									ch.holdFrames = -1; // next start burns a fresh hold
								}
								break; // silence for the rest of this block
							}
							if (!ch.playing && ch.holdFrames < 0)
								ch.holdFrames = holdInit; // fresh start: arm the jitter hold once
							if (ch.holdFrames > 0) {
								int m = std::min(BLOCK - i, ch.holdFrames);
								ch.holdFrames -= m;
								i += m;
								continue;
							}
							// Arrival lock: the interval starts on this very frame.
							ch.cur = std::move(ch.ready.front());
							ch.ready.pop_front();
							ch.curPos = 0;
							ch.playing = true;
							ch.holdFrames = -1;
							if (ch.cur.empty())
								ch.silenceLeft = N; // silence interval placeholder
							else
								audioStarted_.store(true, std::memory_order_relaxed);
							if (!ch.everStarted) {
								// The chain takes over from the join-gap preview: fade
								// the preview's unplayed tail instead of hard-cutting
								// (the interval now replays in its proper slot — a
								// loop-point handover).
								ch.everStarted = true;
								ch.vstarted = false;
								size_t availF = (ch.vfifo.size() - ch.vhead) / 2;
								ch.pfade = availF > 0 ? pfadeTotal : 0;
								if (!ch.pfade) {
									ch.vfifo.clear();
									ch.vhead = 0;
								}
							}
							continue;
						}
						int frames = (int) (ch.cur.size() / 2);
						int m = std::min(BLOCK - i, frames - (int) ch.curPos);
						for (int k = 0; k < m && out; k++) {
							out[(size_t) (i + k) * RING_CH]     += ch.cur[(ch.curPos + (size_t) k) * 2] * ch.gainL;
							out[(size_t) (i + k) * RING_CH + 1] += ch.cur[(ch.curPos + (size_t) k) * 2 + 1] * ch.gainR;
						}
						ch.curPos += (size_t) m;
						i += m;
						if ((int) ch.curPos >= frames) {
							ch.cur.clear(); // done: loop pops the next ready interval (chained)
							ch.curPos = 0;
						}
					}
					// ---- Join-gap preview / its fade-out (mixes over the whole block,
					// independent of the chained playhead above) ----
					if (!ch.everStarted) {
						// Live preview: play the in-flight interval as it streams in
						// (pushdataFeed fills vfifo). Prebuffer before starting; a dry
						// spell re-arms the prebuffer, like voice.
						size_t availF = (ch.vfifo.size() - ch.vhead) / 2;
						if (!ch.vstarted && availF >= previewPrebuf)
							ch.vstarted = true;
						if (ch.vstarted) {
							int m = (int) std::min<size_t>(availF, (size_t) BLOCK);
							for (int k = 0; k < m && out; k++) {
								out[(size_t) k * RING_CH]     += ch.vfifo[ch.vhead + (size_t) k * 2] * ch.gainL;
								out[(size_t) k * RING_CH + 1] += ch.vfifo[ch.vhead + (size_t) k * 2 + 1] * ch.gainR;
							}
							ch.vhead += (size_t) m * 2;
							if (m > 0)
								audioStarted_.store(true, std::memory_order_relaxed);
							if (m < BLOCK)
								ch.vstarted = false; // ran dry — rebuffer before resuming
						}
					} else if (ch.pfade > 0) {
						size_t availF = (ch.vfifo.size() - ch.vhead) / 2;
						int m = (int) std::min<size_t>(std::min(availF, (size_t) BLOCK), (size_t) ch.pfade);
						for (int k = 0; k < m && out; k++) {
							float g = (float) (ch.pfade - k) / (float) pfadeTotal;
							out[(size_t) k * RING_CH]     += ch.vfifo[ch.vhead + (size_t) k * 2] * ch.gainL * g;
							out[(size_t) k * RING_CH + 1] += ch.vfifo[ch.vhead + (size_t) k * 2 + 1] * ch.gainR * g;
						}
						ch.vhead += (size_t) m * 2;
						ch.pfade -= m;
						if (ch.pfade <= 0 || (size_t) m >= availF) {
							ch.vfifo.clear(); // fade done (or fifo dry): preview retired
							ch.vhead = 0;
							ch.pfade = 0;
						}
					}
				}
				++it;
			}
			refreshSlots(); // free departed users' slots; update poly channel count
		}
		nActive.store(active, std::memory_order_relaxed);

		if (!anything) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}
		// Push the block; ring backpressure paces us to the audio thread's real time.
		for (int i = 0; i < BLOCK; i++) {
			while (!ring.push(&block[(size_t) i * RING_CH])) {
				if (abort.load(std::memory_order_acquire))
					return;
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
		}
	}
}

} // namespace nj
} // namespace akaudio
