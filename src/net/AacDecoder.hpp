// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrey Kozlov

#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

// Streaming ADTS-AAC decoder.
//
// macOS only: it uses the system AudioToolbox (AudioFileStream + AudioConverter),
// so there is no extra dependency to bundle. On any other platform available()
// returns false and init() fails, so the caller can report "AAC needs macOS"
// instead of mis-decoding. The codec itself is chosen dynamically at runtime
// from the stream's Content-Type — this just provides the macOS AAC path.
//
// Push model: feed() raw stream bytes as they arrive; decoded interleaved-stereo
// Float32 PCM is delivered through onPCM at the stream's native sample rate (the
// caller resamples to the engine rate, same as the MP3 path).

namespace akaudio {

class AacDecoder {
public:
	AacDecoder() = default;
	~AacDecoder();

	AacDecoder(const AacDecoder&) = delete;
	AacDecoder& operator=(const AacDecoder&) = delete;

	// Is the AAC path compiled in on this platform (i.e. macOS)?
	static bool available();

	// pcm = interleaved L,R; frames = samples per channel; srcRate = stream Hz.
	std::function<void(const float* pcm, int frames, double srcRate)> onPCM;

	bool init();                              // false if unavailable / open failed
	bool feed(const uint8_t* data, size_t n); // false on fatal decode error
	void close();

	// Opaque implementation (AudioToolbox state); defined in AacDecoder.cpp.
	// Public only so the file-local CoreAudio callbacks can name the type.
	struct Impl;

private:
	Impl* impl = nullptr;
};

} // namespace akaudio
