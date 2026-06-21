#pragma once
#include <string>

// Tiny blocking HTTP/1.0 GET for small text bodies (e.g. the NINJAM room
// directory JSON). Plain http:// only, same scope as StreamClient. Reads the
// whole response body into a string — meant for kilobyte-sized API replies,
// NOT for audio streams (those go through StreamClient). MUST be called off the
// UI/audio thread: it blocks on connect/recv.

namespace akaudio {

// GET the url; on success returns true and fills out with the response body.
// On any failure returns false (out is cleared). Follows no redirects.
bool httpGet(const std::string& url, std::string& out);

} // namespace akaudio
