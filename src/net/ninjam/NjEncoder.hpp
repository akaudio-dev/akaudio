// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
// OGG Vorbis encoder for NINJAM transmit. Each NINJAM interval is an independent,
// self-contained OGG stream (headers + audio + end-of-stream). The encoder is
// incremental so txLoop can STREAM an interval while it is being captured — feed()
// frames as they arrive, take() whatever pages libvorbis has finished, and the
// upload goes out in chunks during the interval instead of as one blob after it
// (matching the canonical njclient, and saving a full interval of latency).
// Uses the vendored libvorbis + libogg (src/dep/libvorbis, src/dep/libogg).
#include <cstdint>
#include <vector>

#include <vorbis/codec.h>

namespace akaudio {
namespace nj {

// Incremental per-interval OGG Vorbis encoder. Lifecycle:
//   begin(...) → feed(frames)* → finish() → (take() after any of these)
// take() returns and clears the bytes produced so far; call it after feed()s to
// stream, and after finish() for the tail (EOS pages). A failed begin() leaves the
// encoder inactive; feed/finish/take are then no-ops. Not thread-safe — single
// owner (txLoop).
class OggIntervalEncoder {
public:
	OggIntervalEncoder() = default;
	~OggIntervalEncoder() { reset(); }

	OggIntervalEncoder(const OggIntervalEncoder&) = delete;
	OggIntervalEncoder& operator=(const OggIntervalEncoder&) = delete;

	// Start a new interval stream; emits the OGG/Vorbis header pages into the
	// pending buffer. `quality` is VBR quality (-0.1..1.0; ~0.3-0.5 is a good jam
	// bitrate); `serial` is the OGG stream serial (vary per stream). Returns false
	// (and stays inactive) if libvorbis rejects the parameters.
	bool begin(int channels, int sampleRate, float quality, int serial);
	// Encode `frames` of interleaved float samples (±1.0). Finished pages accumulate
	// in the pending buffer.
	void feed(const float* interleaved, int frames);
	// End the stream (flushes the end-of-stream pages into the pending buffer).
	void finish();
	// Move out whatever encoded bytes are pending (may be empty — libvorbis emits
	// pages in bursts).
	std::vector<uint8_t> take();

	bool active() const { return active_; }
	// Abandon any in-flight stream and free codec state (safe to call anytime).
	void reset();

private:
	void pageOut(bool flush);

	bool active_ = false;
	int channels_ = 0;
	vorbis_info vi_;
	vorbis_comment vc_;
	vorbis_dsp_state vd_;
	vorbis_block vb_;
	ogg_stream_state os_;
	std::vector<uint8_t> pending_;
};

// One-shot convenience (used by enc_test and anywhere a whole interval is already
// in memory): begin+feed+finish+take. Returns the complete OGG bytes (empty on
// failure).
std::vector<uint8_t> encodeOggInterval(const float* interleaved, int frames, int channels,
                                       int sampleRate, float quality, int serial);

} // namespace nj
} // namespace akaudio
