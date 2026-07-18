// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "ImageCache.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace akaudio {

namespace {

// Portable "mkdir if missing"; mode is ignored on Windows (no POSIX perms).
inline void makeDir(const std::string& dir) {
#ifdef _WIN32
	::_mkdir(dir.c_str());
#else
	::mkdir(dir.c_str(), 0755);
#endif
}

bool startsWith(const std::string& b, const char* magic, size_t n) {
	if (b.size() < n)
		return false;
	for (size_t i = 0; i < n; i++)
		if ((unsigned char) b[i] != (unsigned char) magic[i])
			return false;
	return true;
}

bool isPng(const std::string& b) { return startsWith(b, "\x89PNG\r\n\x1a\n", 8); }
bool isJpg(const std::string& b) { return startsWith(b, "\xff\xd8\xff", 3); }
bool isGif(const std::string& b) { return startsWith(b, "GIF8", 4); }
bool isIco(const std::string& b) {
	return b.size() >= 6 && (unsigned char) b[0] == 0 && (unsigned char) b[1] == 0
		&& (unsigned char) b[2] == 1 && (unsigned char) b[3] == 0;
}

uint16_t rd16(const std::string& b, size_t o) {
	return (uint8_t) b[o] | ((uint8_t) b[o + 1] << 8);
}
uint32_t rd32(const std::string& b, size_t o) {
	return (uint32_t) (uint8_t) b[o] | ((uint32_t) (uint8_t) b[o + 1] << 8)
		| ((uint32_t) (uint8_t) b[o + 2] << 16) | ((uint32_t) (uint8_t) b[o + 3] << 24);
}
void wr32(std::string& s, uint32_t v) {
	s += (char) (v & 0xff); s += (char) ((v >> 8) & 0xff);
	s += (char) ((v >> 16) & 0xff); s += (char) ((v >> 24) & 0xff);
}
void wr16(std::string& s, uint16_t v) {
	s += (char) (v & 0xff); s += (char) ((v >> 8) & 0xff);
}

bool writeFile(const std::string& path, const std::string& data) {
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f)
		return false;
	size_t n = data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), f);
	std::fclose(f);
	return n == data.size();
}

// Convert an .ico to a stb-loadable payload. Picks the largest/deepest frame; an
// embedded PNG is returned as-is (ext "png"), a DIB frame is wrapped into a BMP
// file (ext "bmp"). Returns false if it can't make sense of the container.
bool icoToImage(const std::string& b, std::string& out, std::string& ext) {
	if (b.size() < 6)
		return false;
	uint16_t count = rd16(b, 4);
	if (count == 0 || b.size() < 6u + 16u * count)
		return false;

	// Choose the best frame: largest area, then highest bit depth.
	int best = -1;
	long bestScore = -1;
	for (uint16_t i = 0; i < count; i++) {
		size_t e = 6 + 16 * i;
		int w = (uint8_t) b[e]; if (w == 0) w = 256;
		int h = (uint8_t) b[e + 1]; if (h == 0) h = 256;
		uint16_t bpp = rd16(b, e + 6);
		uint32_t size = rd32(b, e + 8);
		uint32_t off = rd32(b, e + 12);
		// Bounds-check in 64-bit: off + size can wrap uint32 on hostile input,
		// which would pass the check and make substr() below throw.
		if (size == 0 || (uint64_t) off + (uint64_t) size > (uint64_t) b.size())
			continue;
		long score = (long) w * h * 100 + bpp;
		if (score > bestScore) {
			bestScore = score;
			best = i;
		}
	}
	if (best < 0)
		return false;

	size_t e = 6 + 16 * best;
	uint32_t size = rd32(b, e + 8);
	uint32_t off = rd32(b, e + 12);
	std::string frame = b.substr(off, size);

	// Embedded PNG frame: use directly.
	if (isPng(frame)) {
		out = frame;
		ext = "png";
		return true;
	}

	// Otherwise a BITMAPINFOHEADER DIB. Wrap it in a BMP file header so stb can
	// read it. The DIB's biHeight is doubled (XOR colour rows + AND mask); halve
	// it so stb reads only the colour rows and ignores the trailing mask.
	if (frame.size() < 40)
		return false;
	uint32_t dibSize = rd32(frame, 0);
	if (dibSize < 40 || dibSize > frame.size())
		return false;
	int32_t biHeight = (int32_t) rd32(frame, 8);
	uint16_t bpp = rd16(frame, 14);
	uint32_t clrUsed = rd32(frame, 32);

	int32_t realH = biHeight / 2;
	// Patch biHeight in a mutable copy of the DIB.
	std::string dib = frame;
	dib[8] = (char) (realH & 0xff);
	dib[9] = (char) ((realH >> 8) & 0xff);
	dib[10] = (char) ((realH >> 16) & 0xff);
	dib[11] = (char) ((realH >> 24) & 0xff);

	uint32_t palette = 0;
	if (bpp <= 8) {
		uint32_t maxColors = 1u << bpp;            // ≤ 256 for a palettized DIB
		uint32_t colors = clrUsed ? clrUsed : maxColors;
		if (colors > maxColors)
			colors = maxColors;                    // clamp a bogus/hostile biClrUsed so
		palette = colors * 4;                      // colors*4 (≤1024) can't overflow uint32
	}
	// dibSize is already bounded by frame.size(); palette ≤ 1024 — no overflow here.
	uint32_t dataOff = 14 + dibSize + palette;

	std::string bmp;
	bmp += 'B'; bmp += 'M';
	wr32(bmp, 14 + (uint32_t) dib.size()); // file size
	wr16(bmp, 0); wr16(bmp, 0);            // reserved
	wr32(bmp, dataOff);                    // pixel data offset
	bmp += dib;

	out = bmp;
	ext = "bmp";
	return true;
}

} // namespace

std::string cacheImage(const std::string& bytes, const std::string& dir, const std::string& key) {
	if (bytes.size() < 16)
		return "";

	std::string payload, ext;
	if (isPng(bytes)) { payload = bytes; ext = "png"; }
	else if (isJpg(bytes)) { payload = bytes; ext = "jpg"; }
	else if (isGif(bytes)) { payload = bytes; ext = "gif"; }
	else if (isIco(bytes)) {
		if (!icoToImage(bytes, payload, ext))
			return "";
	}
	else {
		return ""; // not an image we can decode
	}

	makeDir(dir); // ok if it already exists
	std::string path = dir + "/" + key + "." + ext;
	if (!writeFile(path, payload))
		return "";
	return path;
}

} // namespace akaudio
