// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "Hls.hpp"
#include "Http.hpp" // endsWithCI, pathNoQuery, urlJoin — shared across net/

#include <cstdlib>

namespace akaudio {

namespace {

std::string trimCR(const std::string& s) {
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t'))
		a++;
	while (b > a && (s[b - 1] == '\r' || s[b - 1] == ' ' || s[b - 1] == '\t'))
		b--;
	return s.substr(a, b - a);
}

bool startsWith(const std::string& s, const char* p) {
	return s.rfind(p, 0) == 0;
}

} // namespace

HlsPlaylist parseHlsPlaylist(const std::string& body) {
	HlsPlaylist pl;
	bool expectVariant = false; // saw #EXT-X-STREAM-INF; next URI is a variant
	size_t i = 0, n = body.size();
	while (i < n) {
		size_t e = body.find('\n', i);
		std::string line = trimCR(body.substr(i, (e == std::string::npos ? n : e) - i));
		i = (e == std::string::npos ? n : e + 1);
		if (line.empty())
			continue;

		if (line[0] == '#') {
			if (startsWith(line, "#EXT-X-STREAM-INF")) {
				pl.isMaster = true;
				expectVariant = true;
			}
			else if (startsWith(line, "#EXT-X-MEDIA-SEQUENCE:")) {
				pl.mediaSequence = std::strtoull(line.c_str() + 22, nullptr, 10);
			}
			else if (startsWith(line, "#EXT-X-TARGETDURATION:")) {
				pl.targetDuration = std::atof(line.c_str() + 22);
			}
			else if (startsWith(line, "#EXT-X-ENDLIST")) {
				pl.endList = true;
			}
			continue;
		}

		// A URI line.
		if (expectVariant) {
			if (pl.variant.empty())
				pl.variant = line; // take the first variant
			expectVariant = false;
		}
		else if (!pl.isMaster) {
			pl.segments.push_back(line);
		}
	}
	return pl;
}

bool looksLikeHls(const std::string& url) {
	return endsWithCI(pathNoQuery(url), ".m3u8");
}

void tsExtractAdts(const uint8_t* d, size_t n, std::string& out) {
	int audioPid = -1;
	for (size_t i = 0; i + 188 <= n; i += 188) {
		const uint8_t* p = d + i;
		if (p[0] != 0x47)
			continue; // not a TS sync byte; skip (best-effort)

		bool pusi = (p[1] & 0x40) != 0;          // payload unit start
		int pid = ((p[1] & 0x1F) << 8) | p[2];
		int afc = (p[3] >> 4) & 0x3;             // adaptation field control
		if (!(afc & 0x1))
			continue; // no payload

		size_t off = 4;
		if (afc & 0x2)
			off += 1 + p[4]; // skip adaptation field
		if (off >= 188)
			continue;
		const uint8_t* payload = p + off;
		size_t plen = 188 - off;

		// Start of a PES packet?
		if (pusi && plen >= 9 && payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) {
			uint8_t streamId = payload[3];
			if (streamId >= 0xC0 && streamId <= 0xDF) { // audio stream
				if (audioPid < 0)
					audioPid = pid;
				if (pid == audioPid) {
					size_t hdr = 9 + payload[8]; // PES header: 9 fixed + header_data_length
					if (hdr < plen)
						out.append(reinterpret_cast<const char*>(payload + hdr), plen - hdr);
				}
				continue;
			}
		}
		// Continuation of the audio PES.
		if (pid == audioPid && !pusi)
			out.append(reinterpret_cast<const char*>(payload), plen);
	}
}

void hlsSegmentToAdts(const uint8_t* d, size_t n, std::string& out) {
	if (n >= 1 && d[0] == 0x47) { // MPEG-TS sync byte
		tsExtractAdts(d, n, out);
		return;
	}
	// Raw AAC segment: skip any leading ID3v2 tags (10-byte header with a
	// syncsafe 28-bit size), then require an ADTS syncword before appending.
	size_t off = 0;
	while (n - off >= 10 && d[off] == 'I' && d[off + 1] == 'D' && d[off + 2] == '3') {
		size_t sz = ((size_t)(d[off + 6] & 0x7f) << 21) | ((size_t)(d[off + 7] & 0x7f) << 14)
		          | ((size_t)(d[off + 8] & 0x7f) << 7) | (size_t)(d[off + 9] & 0x7f);
		off += 10 + sz;
		if (off > n) {
			off = n;
			break;
		}
	}
	if (n - off >= 2 && d[off] == 0xff && (d[off + 1] & 0xf0) == 0xf0)
		out.append(reinterpret_cast<const char*>(d) + off, n - off);
}

} // namespace akaudio
