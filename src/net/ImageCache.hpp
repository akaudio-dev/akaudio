#pragma once
#include <string>

// Normalize a downloaded favicon into a file that Rack's image loader (stb_image
// under NanoVG) can decode, cached on disk for reuse.
//
// stb decodes PNG / JPEG / GIF / BMP but NOT the .ico container, and most site
// favicons are .ico. cacheImage() therefore detects the format from magic bytes:
// PNG/JPG/GIF are written through verbatim; an .ico is parsed (its best frame is
// either an embedded PNG, written as-is, or a DIB/BMP frame, wrapped in a
// BITMAPFILEHEADER) so it lands as a stb-loadable file. Returns "" if the bytes
// aren't a usable image. Rack-free (std file I/O only), so the test harnesses
// that link net/ without libRack still build.

namespace akaudio {

// Write `bytes` (a downloaded image) into `dir`/`key`.<ext>, converting .ico as
// needed. Creates `dir` if missing. Returns the absolute path written, or "" on
// an unrecognised/undecodable image.
std::string cacheImage(const std::string& bytes, const std::string& dir, const std::string& key);

} // namespace akaudio
