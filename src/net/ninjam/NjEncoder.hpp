// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
// OGG Vorbis encoder for NINJAM transmit. Encodes one complete interval of interleaved
// audio into a self-contained OGG stream (headers + audio + end-of-stream) — matching how
// NINJAM ships each interval (every interval is independently decodable). Uses the vendored
// libvorbis + libogg (src/dep/libvorbis, src/dep/libogg).
#include <cstdint>
#include <vector>

namespace akaudio {
namespace nj {

// Encode `frames` of interleaved float samples (channels interleaved, ±1.0) at sampleRate
// into a complete OGG Vorbis stream. `quality` is VBR quality (-0.1 .. 1.0; ~0.3-0.5 is a
// good jam bitrate). `serial` is the OGG stream serial number (any value; vary per stream).
// Returns the OGG bytes (empty on failure).
std::vector<uint8_t> encodeOggInterval(const float* interleaved, int frames, int channels,
                                       int sampleRate, float quality, int serial);

} // namespace nj
} // namespace akaudio
