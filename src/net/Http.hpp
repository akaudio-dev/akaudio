#pragma once
#include <string>
#include <atomic>

// Tiny HTTP(S)/1.0 GET for small text/binary bodies (room/station directory JSON,
// playlists, favicons). Reads the whole response into a string — meant for
// kilobyte-sized replies, NOT for audio streams (those go through StreamClient).
// MUST be called off the UI/audio thread.
//
// Bounded + interruptible: a non-blocking connect and a per-recv timeout mean no
// call can block forever on a dead/silent peer, and if `abort` is supplied it
// bails promptly when set — so a caller that join()s this thread (e.g.
// StreamClient on a station switch) never freezes the UI. Follows redirects.

namespace akaudio {

// GET the url; on success returns true and fills out with the response body. On
// any failure returns false (out cleared). If abort is non-null and becomes true,
// the request is abandoned and false returned.
bool httpGet(const std::string& url, std::string& out, const std::atomic<bool>* abort = nullptr);

} // namespace akaudio
