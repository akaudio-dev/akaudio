// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

// Offline test for looper::AlsExport (docs/LOOPER_DESIGN.md §12): build an .als from
// synthetic jam data and check the XML (tracks, clips, escaping, beat positions) and the
// gzip framing (a stored-block decoder reconstructs the XML byte-for-byte — proving any
// inflater, including Live's, recovers it). No Rack link, no zlib.
//   c++ -std=c++11 -O1 -I src test/als_export_test.cpp src/looper/AlsExport.cpp -o build/als_export_test && build/als_export_test
#include "looper/AlsExport.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace akaudio::looper;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static bool has(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }
static int count(const std::string& h, const std::string& n) {
	int c = 0; size_t p = 0;
	while ((p = h.find(n, p)) != std::string::npos) { c++; p += n.size(); }
	return c;
}

// Inflate a gzip made only of stored DEFLATE blocks (what gzipStore emits). Returns the
// payload; sets ok=false on any framing error.
static std::string gunzipStored(const std::vector<uint8_t>& g, bool& ok) {
	ok = false;
	std::string out;
	if (g.size() < 18 || g[0] != 0x1f || g[1] != 0x8b || g[2] != 0x08) return out;
	size_t p = 10; // past the fixed header (no extra flags set by gzipStore)
	for (;;) {
		if (p + 5 > g.size()) return out;
		uint8_t bfinal = g[p] & 1, btype = (g[p] >> 1) & 3;
		if (btype != 0) return out; // only stored blocks
		p++;
		uint16_t len = g[p] | (g[p + 1] << 8);
		uint16_t nlen = g[p + 2] | (g[p + 3] << 8);
		if ((uint16_t) ~len != nlen) return out;
		p += 4;
		if (p + len > g.size()) return out;
		out.append((const char*) &g[p], len);
		p += len;
		if (bfinal) break;
	}
	ok = true; // trailer (crc + isize) follows; the payload is what we return
	return out;
}

int main() {
	AlsProject p;
	p.title = "jam";
	p.tempo = 120.0;
	p.meterNum = 4;
	for (int t = 0; t < 8; t++) p.looperTrackNames[t] = "inst<" + std::to_string(t) + ">"; // '<' exercises escaping

	AlsSessionClip a;
	a.track = 0; a.scene = 0; a.name = "take A&B"; a.absPath = "/home/u/jam/looper/t0_s0.ogg";
	a.relPath = "looper/t0_s0.ogg"; a.frames = 48000; a.sampleRate = 48000; a.bpm = 120; a.fileSize = 1234;
	p.sessionClips.push_back(a);
	AlsSessionClip b = a; b.track = 3; b.scene = 5; b.name = "riff"; b.absPath = "/home/u/jam/looper/t3_s5.ogg";
	p.sessionClips.push_back(b);

	AlsArrangementTrack tr;
	tr.name = "bob";
	AlsArrangementClip c1;
	c1.name = "bob 0"; c1.absPath = "/home/u/jam/players/000000_bob_ch0.ogg"; c1.relPath = "players/000000_bob_ch0.ogg";
	c1.sessionFrame = 48000; c1.frames = 48000; c1.sampleRate = 48000; c1.fileSize = 999; // 1 s → beat 2 @120
	tr.clips.push_back(c1);
	AlsArrangementClip c2 = c1; c2.name = "bob 1"; c2.sessionFrame = 96000; // 2 s → beat 4
	tr.clips.push_back(c2);
	p.arrangementTracks.push_back(tr);
	AlsArrangementTrack tx; tx.name = "you (tx)"; tx.isTx = true; tx.clips.push_back(c1);
	p.arrangementTracks.push_back(tx);

	std::string xml = buildAlsXml(p);

	// ---- Structure ----
	CHECK(has(xml, "<?xml version=\"1.0\""), "xml declaration");
	CHECK(has(xml, "<Ableton MajorVersion=\"5\" MinorVersion=\"11"), "Live 11 Ableton header");
	CHECK(has(xml, "</Ableton>"), "closed Ableton");
	CHECK(count(xml, "<LiveSet>") == 1 && count(xml, "</LiveSet>") == 1, "one balanced LiveSet");
	CHECK(count(xml, "<AudioTrack ") == 10, "8 looper + 2 arrangement tracks (got %d)", count(xml, "<AudioTrack "));
	CHECK(count(xml, "<AudioTrack ") == count(xml, "</AudioTrack>"), "AudioTrack tags balanced");
	// 2 session clips + (2 + 1) arrangement clips = 5.
	CHECK(count(xml, "<AudioClip ") == 5, "5 audio clips total (got %d)", count(xml, "<AudioClip "));
	CHECK(count(xml, "<AudioClip ") == count(xml, "</AudioClip>"), "AudioClip tags balanced");
	CHECK(count(xml, "<ClipSlot ") == 80, "8 tracks-with-slots... (got %d)", count(xml, "<ClipSlot ")); // 10 tracks × 8

	// ---- Content ----
	CHECK(has(xml, "Master"), "master track present");
	CHECK(has(xml, "Value=\"120\""), "set tempo 120 present");
	CHECK(has(xml, ">bob<") || has(xml, "\"bob\""), "arrangement track 'bob'");
	CHECK(has(xml, "you (tx)"), "TX track name");
	CHECK(has(xml, "/home/u/jam/players/000000_bob_ch0.ogg"), "absolute sample path referenced");
	CHECK(has(xml, "players/000000_bob_ch0.ogg\" />"), "relative sample path referenced");

	// ---- Escaping ----
	CHECK(has(xml, "take A&amp;B"), "'&' escaped in a clip name");
	CHECK(has(xml, "inst&lt;0&gt;"), "'<' '>' escaped in a track name");
	CHECK(!has(xml, "inst<0>"), "no raw unescaped '<' from a name");
	CHECK(!has(xml, "take A&B"), "no raw unescaped '&' from a name");

	// ---- Beat positions (unwarped, tempo 120) ----
	CHECK(has(xml, "<AudioClip Id=") , "audio clip ids");
	CHECK(has(xml, "Time=\"2\""), "clip at 1 s lands on beat 2 @120 BPM");
	CHECK(has(xml, "Time=\"4\""), "clip at 2 s lands on beat 4 @120 BPM");

	// ---- gzip framing ----
	std::vector<uint8_t> gz = gzipStore(xml);
	CHECK(gz.size() > 18 && gz[0] == 0x1f && gz[1] == 0x8b && gz[2] == 0x08, "gzip magic + deflate method");
	uint32_t isize = gz[gz.size() - 4] | (gz[gz.size() - 3] << 8) | (gz[gz.size() - 2] << 16) | ((uint32_t) gz[gz.size() - 1] << 24);
	CHECK(isize == xml.size(), "gzip ISIZE trailer == payload length (%u vs %zu)", isize, xml.size());
	bool ok = false;
	std::string back = gunzipStored(gz, ok);
	CHECK(ok, "gzip stream is well-framed (stored blocks)");
	CHECK(back == xml, "inflate reconstructs the XML byte-for-byte (%zu vs %zu)", back.size(), xml.size());

	// Empty payload still yields a valid gzip (a final empty stored block).
	std::vector<uint8_t> ge = gzipStore("");
	bool oke = false; std::string be = gunzipStored(ge, oke);
	CHECK(oke && be.empty(), "empty input → valid empty gzip");

	std::printf("%s (%d failures)\n", fails ? "FAIL" : "PASS: AlsExport", fails);
	return fails ? 1 : 0;
}
