// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
// JamExport — reconstruct a recorded jam folder as an Ableton Live set (.als), the
// Rack-side glue over looper/AlsExport (docs/LOOPER_DESIGN.md §12). Owned by the
// Recorder module (context menu + auto-export on disarm); moved out of Looper.cpp so a
// jam recorded without any Looper exports too.
//
// Reads, all optional (at least one must yield content):
//   <jamRoot>/looper/session.json  — the Looper grid → Session-View clips
//   <jamRoot>/looper/events.jsonl  — the performance log → per-track "as played"
//                                    Arrangement lanes (play-spans looping their take)
//   <jamRoot>/index.jsonl          — the wire archive → per-player + TX Arrangement
//                                    tracks, every interval at its sessionFrame
// Writes <jamRoot>/<basename>.als + the empty "Ableton Project Info/" marker folder
// (without it Live won't resolve the project-relative sample paths). Returns the .als
// path, or "" with the reason in *whyNot. UI thread (jansson + file I/O).
#include <string>

namespace akaudio {

// `liteMode` targets Ableton Live Lite's 8-track cap: the grid exports 6 tracks
// (takes on tracks 7-8 are skipped with a warning), every remote player is merged onto
// ONE arrangement lane (overlapping intervals are audio-mixed into <jamRoot>/mixdown/;
// lone intervals reference their original OGG untouched), and the players + TX lanes
// land in template tracks 7+8 instead of cloned tracks — exactly 8 audio tracks.
std::string exportJamAls(const std::string& jamRoot, bool liteMode = false,
                         std::string* whyNot = nullptr);

} // namespace akaudio
