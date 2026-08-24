// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
// SessionMirror — incrementally sync the LIVE cells of a looper session folder into
// another folder (docs/LOOPER_DESIGN.md; "embed takes in patch"). The session folder on
// disk is always the source of truth; a mirror is a detachable snapshot of it — the
// Looper mirrors session → Rack patch storage on every patch save, and patch storage →
// a fresh session folder when a patch loads with its original folder gone (a shared
// patch, a deleted jams dir).
//
// Only the live grid travels: each t<t>_s<s>.ogg (Session::liveName) plus session.json.
// history/ and anything else in the folder is never read or written. Copies preserve
// the source mtime, and a file is recopied only when size or mtime differ — so an
// unchanged grid is a no-op (autosave hits this every ~15 s), and a mirror hydrated
// from a snapshot mirrors back as 0 changes.
//
// Rack-free (portable stdio/stat), UI-thread callers; safe against a concurrent worker
// commit because Session writes every file atomically (tmp + rename).
#include <string>

namespace akaudio {
namespace looper {

// Sync src's live cells + manifest into dst (created if needed): copy each cell whose
// size/mtime differ, delete dst cells absent from src, same rule for session.json.
// Returns the number of files copied or deleted, or -1 when src has no session.json —
// in that case nothing is touched and dst is not created.
int mirrorSession(const std::string& srcDir, const std::string& dstDir);

// Remove a mirror's live cells + session.json from dst (an "embed takes" opt-out).
// Everything else in dst is left alone. Returns the number of files deleted.
int clearSessionMirror(const std::string& dstDir);

} // namespace looper
} // namespace akaudio
