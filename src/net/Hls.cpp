#include "Hls.hpp"

#include <cctype>
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

bool endsWithCI(const std::string& s, const std::string& suf) {
	if (s.size() < suf.size())
		return false;
	for (size_t i = 0; i < suf.size(); i++)
		if (std::tolower((unsigned char) s[s.size() - suf.size() + i]) != std::tolower((unsigned char) suf[i]))
			return false;
	return true;
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

std::string urlJoin(const std::string& base, const std::string& ref) {
	if (startsWith(ref, "http://") || startsWith(ref, "https://"))
		return ref;
	if (startsWith(ref, "//")) { // scheme-relative
		size_t s = base.find("://");
		std::string scheme = (s == std::string::npos) ? "http:" : base.substr(0, s + 1);
		return scheme + ref;
	}
	if (!ref.empty() && ref[0] == '/') { // host-rooted
		size_t s = base.find("://");
		if (s != std::string::npos) {
			size_t h = base.find('/', s + 3);
			std::string origin = (h == std::string::npos) ? base : base.substr(0, h);
			return origin + ref;
		}
	}
	// relative to the playlist's directory
	size_t slash = base.rfind('/');
	return (slash == std::string::npos ? base : base.substr(0, slash + 1)) + ref;
}

bool looksLikeHls(const std::string& url, const std::string& body) {
	// strip a query string before the extension check
	std::string path = url;
	size_t q = path.find('?');
	if (q != std::string::npos)
		path = path.substr(0, q);
	if (endsWithCI(path, ".m3u8"))
		return true;
	return startsWith(trimCR(body.substr(0, body.find('\n') == std::string::npos ? body.size() : body.find('\n'))), "#EXTM3U");
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

} // namespace akaudio
