// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
// AlsExport — turn a recorded jam into an Ableton Live Set (.als), the payoff of the
// shared session timeline (docs/LOOPER_DESIGN.md §12). Attempt #2, template-based:
// instead of synthesizing Live's XML from scratch (attempt #1, reverted — Live's loader
// crashes on anything not byte-shaped like its own output), we perform targeted surgery
// on a real set that Live 11 itself saved (res/als/Live11Template.xml, gunzipped from
// refs/Live11 Project/Live11.als: 8 audio tracks × 8 scenes, 2 returns — exactly the
// Looper grid). Everything Live is strict about — the ~1000-line per-track device
// serialization, the pointee-ID graph, mixers, routings — is Live's own bytes, untouched.
// We only: rename the 8 tracks, clear the template's demo clips, insert our take clips
// into their slots (warped, looping, referencing the raw take OGGs — Live imports Ogg
// Vorbis, no re-encode), patch the set tempo, and optionally clone the (cleared) donor
// track for Arrangement-View player/TX tracks, renumbering only the cloned
// AutomationTarget/ModulationTarget ids from NextPointeeId.
//
// Two views, reconstructing the whole jam:
//   * Session View  — the 8 Looper tracks, each take an Ogg clip in its scene slot (loops).
//   * Arrangement   — one audio track per remote player and our TX, every interval clip
//                     placed at its `sessionFrame` (the session as heard here).
//
// Pure data in (already parsed from session.json + index.jsonl by the Rack module) → the
// .als bytes out. Rack-free, no jansson, no zlib (gzip is emitted with stored DEFLATE
// blocks + CRC32), so it links into test/als_export_test.cpp with nothing extra.
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
	int bpi = 0;            // beats per interval — the exact loop length; 0 = derive
	long fileSize = 0;
	long mtime = 0;         // the OGG's mtime (epoch seconds) — Live's FileRef LastModDate
};

// A received/sent interval → a one-shot clip on the Arrangement timeline; or — with
// `loop` set — a Looper play-span: a clip stretched from `sessionFrame` to
// `endSessionFrame` looping its take inside the span (`loopLenBeats` beats per cycle),
// the shape Live's own session→arrangement recording produces.
struct AlsArrangementClip {
	std::string name;
	std::string absPath;
	std::string relPath;
	uint64_t sessionFrame = 0;    // start on the shared timeline (frames since join)
	uint64_t endSessionFrame = 0; // loop spans only (0 ⇒ one-shot: span = `frames`)
	bool loop = false;
	double loopLenBeats = 0;      // loop cycle length in beats (loop spans only)
	long frames = 0;
	double sampleRate = 48000.0;
	long fileSize = 0;
	long mtime = 0;
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
	// Subtracted from every Arrangement position: the session clock runs from the JOIN,
	// so a jam armed minutes into a room would otherwise start 100+ empty bars in.
	// Callers set it to the earliest archived frame, rounded down to a whole interval
	// so downbeats stay on bars (JamExport does this).
	uint64_t timelineOrigin = 0;
	// How many template tracks belong to the Looper grid (8 normally; 6 in the Live
	// Lite flavor, freeing template tracks 7+8 for the inline arrangement below).
	int looperTracks = 8;
	// With `inlineArrangement`, arrangementTracks land in the template tracks AFTER the
	// grid (renamed to the lane's name) instead of being cloned — the Live Lite flavor,
	// whose 8-track cap forbids clones. arrangementTracks.size() must fit 8-looperTracks.
	bool inlineArrangement = false;
	std::string looperTrackNames[8];
	std::vector<AlsSessionClip> sessionClips;
	std::vector<AlsArrangementTrack> arrangementTracks;
	// The Looper's as-played spans, per grid track — placed on that template track's OWN
	// Arrangement lane (a track holds its session clips and its timeline; no clones).
	std::vector<AlsArrangementClip> looperLanes[8];
};

// ---- Looper "as played" lanes (docs §12) ------------------------------------------
// Parsed inputs, kept jansson-free so this stays Rack-free and unit-testable: the Rack
// glue (JamExport) parses events.jsonl / session.json into these.

// One performance event row from events.jsonl.
struct LoopEventIn {
	bool start = false;
	int track = 0, slot = 0;
	uint64_t sessionFrame = 0;
	uint64_t takeStartFrame = 0; // identity of the audio playing (see take-identity rule)
	bool rest = false;
	int bpm = 0, bpi = 0;
	double sampleRate = 48000.0;
};

// One take from session.json's slots (the grid as it stands now).
struct LooperTakeIn {
	int track = 0, slot = 0;
	std::string file;            // "t<t>_s<s>.ogg" (relative to the looper/ dir)
	std::string absPath;         // the same file, absolute (filled by the caller)
	long frames = 0;
	double sampleRate = 48000.0;
	int bpm = 0, bpi = 0;
	uint64_t startFrame = 0;     // capture identity
	long fileSize = 0;
	long mtime = 0;
};

// Reconstruct the Looper's as-played timeline into out.looperLanes[t] (each grid
// track's own Arrangement lane; tracks >= out.looperTracks are skipped). Rules:
//   * tracks are monophonic — a START closes any open span on that track at the same
//     frame (robust to a lost STOP); an unpaired START closes at `timelineEnd`;
//   * a span becomes a clip only when a take still exists with the same (track, slot)
//     AND startFrame == the event's takeStartFrame — re-recorded audio (now in
//     history/, no metadata) is skipped. Overdubs keep startFrame, so pre-overdub
//     spans deliberately reference the final overdubbed audio (Live's "clip as it now
//     is" semantics). Rest spans (silence steps) and zero-length spans are skipped;
//   * events with bpm <= 0 (simulated clock) fall back to `tempo` for beat math.
void buildLooperLanes(const std::vector<LoopEventIn>& events,
                      const std::vector<LooperTakeIn>& takes,
                      const std::string trackNames[8], uint64_t timelineEnd,
                      double tempo, AlsProject& out);

// The XML text of the set (uncompressed) — the template with our jam spliced in.
// Empty on failure (template doesn't look like the expected Live 11 set), with the
// reason in *err if given.
std::string buildAlsXml(const AlsProject& p, const std::string& templateXml,
                        std::string* err = nullptr);
// gzip a buffer using stored (uncompressed) DEFLATE blocks — a valid .gz that Live
// inflates. Self-contained: no zlib.
std::vector<uint8_t> gzipStore(const std::string& data);
// The finished .als bytes (gzip of buildAlsXml); empty on failure.
inline std::vector<uint8_t> buildAls(const AlsProject& p, const std::string& templateXml,
                                     std::string* err = nullptr) {
	std::string xml = buildAlsXml(p, templateXml, err);
	return xml.empty() ? std::vector<uint8_t>() : gzipStore(xml);
}

} // namespace looper
} // namespace akaudio
