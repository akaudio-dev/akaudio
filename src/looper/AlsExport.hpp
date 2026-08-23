// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
// AlsExport — turn a recorded jam into an Ableton Live Set (.als), the payoff of the
// shared session timeline (docs/LOOPER_DESIGN.md §12). A .als is gzipped XML and Live
// imports Ogg Vorbis, so we reference the raw take / interval OGGs directly — no re-encode.
//
// Two views, reconstructing the whole jam:
//   * Session View  — the 8 Looper tracks, each take an Ogg clip in its scene slot (loops).
//   * Arrangement   — one audio track per remote player and our TX, every interval clip
//                     placed at its `sessionFrame` (the session as heard here).
//
// Pure data in (already parsed from session.json + index.jsonl by the Rack module) → the
// .als bytes out. Rack-free, no jansson, no zlib (gzip is emitted with stored DEFLATE
// blocks + CRC32), so it links into test/als_export_test.cpp with nothing extra.
//
// NOTE: the .als schema is Ableton's, reverse-engineered; this targets Live 11. Live is
// strict about it, and this can't be validated without a Live install — expect to test it
// in Live and iterate. Absolute sample paths are primary (a local artifact), relative
// backup.
#include <cstdint>
#include <string>
#include <vector>

namespace akaudio {
namespace looper {

// A Looper take → a looping clip in a Session-View slot.
struct AlsSessionClip {
	int track = 0;          // Live track index (0..7, the Looper track)
	int scene = 0;          // clip-slot / scene row (0..7, the Looper slot)
	std::string name;
	std::string absPath;    // absolute path to the take OGG
	std::string relPath;    // path relative to the .als (fallback)
	long frames = 0;
	double sampleRate = 48000.0;
	double bpm = 120.0;     // the take's tempo (for its loop length in beats)
	long fileSize = 0;
};

// A received/sent interval → a one-shot clip on the Arrangement timeline.
struct AlsArrangementClip {
	std::string name;
	std::string absPath;
	std::string relPath;
	uint64_t sessionFrame = 0; // start on the shared timeline (frames since join)
	long frames = 0;
	double sampleRate = 48000.0;
	long fileSize = 0;
};

// One Arrangement track = one player (or our TX), with its intervals.
struct AlsArrangementTrack {
	std::string name;
	bool isTx = false;
	std::vector<AlsArrangementClip> clips;
};

struct AlsProject {
	std::string title;
	double tempo = 120.0;   // global set tempo (BPM)
	int meterNum = 4;       // time-signature numerator
	std::string looperTrackNames[8];
	std::vector<AlsSessionClip> sessionClips;
	std::vector<AlsArrangementTrack> arrangementTracks;
};

// The XML text of the set (uncompressed) — exposed for testing/inspection.
std::string buildAlsXml(const AlsProject& p);
// gzip a buffer using stored (uncompressed) DEFLATE blocks — a valid .gz that Live
// inflates. Self-contained: no zlib.
std::vector<uint8_t> gzipStore(const std::string& data);
// The finished .als bytes (gzip of buildAlsXml).
inline std::vector<uint8_t> buildAls(const AlsProject& p) { return gzipStore(buildAlsXml(p)); }

} // namespace looper
} // namespace akaudio
