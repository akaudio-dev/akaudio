// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// HLS (HTTP Live Streaming) helpers: playlist parsing, URL joining, and an
// MPEG-TS → AAC-ADTS demux. Pure functions, no I/O — the polling loop and the
// AAC decode live in StreamClient (macOS), which calls these.
//
// HLS isn't a byte stream: an .m3u8 lists short media segments that the client
// fetches in sequence (re-reading the playlist for new ones on a live feed).
// Modern broadcasters (e.g. BBC) are HLS-only, with AAC inside MPEG-TS segments.

namespace akaudio {

struct HlsPlaylist {
	bool isMaster = false;            // a master (variant) playlist vs a media playlist
	bool endList = false;             // #EXT-X-ENDLIST present (VOD; stop polling)
	double targetDuration = 6.0;      // #EXT-X-TARGETDURATION (poll cadence hint)
	uint64_t mediaSequence = 0;       // #EXT-X-MEDIA-SEQUENCE (sequence of segments[0])
	std::string variant;              // master: chosen variant playlist URI
	std::vector<std::string> segments; // media: segment URIs, in order
};

// Parse an .m3u8 body into the struct above.
HlsPlaylist parseHlsPlaylist(const std::string& body);

// (urlJoin — resolving a relative playlist/segment ref — lives in Http.hpp now.)

// Does this look like HLS? (.m3u8 path, query string ignored.)
bool looksLikeHls(const std::string& url);

// Append the AAC ADTS elementary stream carried in MPEG-TS bytes to out.
// Single-audio-program demux: finds the audio PES (stream_id 0xC0–0xDF) and
// concatenates its payload (which is ADTS) — no PAT/PMT needed for our feeds.
void tsExtractAdts(const uint8_t* data, size_t n, std::string& out);

// Normalize one HLS media segment to a bare ADTS byte stream: MPEG-TS segments
// (0x47 sync) go through tsExtractAdts; raw AAC/ADTS segments — possibly
// prefixed with ID3v2 timestamp tags, Apple-style (.aac segments, e.g. CBC) —
// are appended directly with the tags stripped. Anything else appends nothing.
void hlsSegmentToAdts(const uint8_t* data, size_t n, std::string& out);

} // namespace akaudio
