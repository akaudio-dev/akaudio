// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov
//
// Offline check of looper/AlsExport (template-based .als generation, docs §12).
// Feeds the real shipped template (res/als/Live11Template.xml) a synthetic jam and
// asserts the surgery: tracks renamed, demo clips gone (no Core Library / .aif refs
// leak), our session clips land in the right slots, arrangement tracks are cloned with
// fresh non-colliding ids, tempo patched, warp markers unique, and the gzip container
// round-trips (stored-DEFLATE inflate + CRC). Build:
//   c++ -std=c++11 -I src test/als_export_test.cpp src/looper/AlsExport.cpp \
//     -o build/als_export_test && build/als_export_test
// Optional argv: [1] template path (default res/als/Live11Template.xml),
//                [2] out dir for the generated .xml/.als (default build/als_export_out).

#include "looper/AlsExport.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace akaudio::looper;

static int failures = 0;
#define CHECK(cond, msg) do { \
	if (cond) { std::printf("  ok: %s\n", msg); } \
	else { std::printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

static std::string readFile(const std::string& p) {
	std::ifstream f(p, std::ios::binary);
	if (!f) return "";
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static size_t countOf(const std::string& s, const std::string& needle) {
	size_t n = 0, p = 0;
	while ((p = s.find(needle, p)) != std::string::npos) { n++; p += needle.size(); }
	return n;
}

// Minimal inflate for our own gzipStore output (stored blocks only) + CRC check.
static std::string gunzipStored(const std::vector<uint8_t>& gz, bool* ok) {
	*ok = false;
	if (gz.size() < 18 || gz[0] != 0x1f || gz[1] != 0x8b || gz[2] != 0x08 || gz[3] != 0)
		return "";
	std::string out;
	size_t p = 10;
	while (true) {
		if (p + 5 > gz.size()) return "";
		uint8_t hdr = gz[p];
		if ((hdr & 0x06) != 0) return "";   // not a stored block
		uint16_t len = (uint16_t) (gz[p + 1] | (gz[p + 2] << 8));
		uint16_t nlen = (uint16_t) (gz[p + 3] | (gz[p + 4] << 8));
		if ((uint16_t) ~len != nlen) return "";
		p += 5;
		if (p + len > gz.size()) return "";
		out.append((const char*) gz.data() + p, len);
		p += len;
		if (hdr & 0x01) break;              // BFINAL
	}
	if (p + 8 > gz.size()) return "";
	uint32_t isize = (uint32_t) gz[p + 4] | ((uint32_t) gz[p + 5] << 8)
	               | ((uint32_t) gz[p + 6] << 16) | ((uint32_t) gz[p + 7] << 24);
	if (isize != (uint32_t) out.size()) return "";
	*ok = true;
	return out;
}

// All numeric values of `<tag` occurrences' Id="N" attributes.
static std::vector<long> idsOf(const std::string& s, const std::string& openPat) {
	std::vector<long> ids;
	size_t p = 0;
	while ((p = s.find(openPat, p)) != std::string::npos) {
		p += openPat.size();
		ids.push_back(std::strtol(s.c_str() + p, nullptr, 10));
	}
	return ids;
}

int main(int argc, char** argv) {
	std::string tplPath = argc > 1 ? argv[1] : "res/als/Live11Template.xml";
	std::string outDir = argc > 2 ? argv[2] : "build/als_export_out";
	std::string tpl = readFile(tplPath);
	if (tpl.empty()) {
		std::printf("FAIL: cannot read template %s (run from the repo root)\n", tplPath.c_str());
		return 1;
	}

	std::printf("== build a synthetic jam over the real template ==\n");
	AlsProject p;
	p.title = "als_test_jam";
	p.tempo = 88.5;
	const char* names[8] = {"synth", "bass & keys", "guitar <lead>", "drums",
	                        "Track 5", "Track 6", "Track 7", "vox"};
	for (int t = 0; t < 8; t++) p.looperTrackNames[t] = names[t];

	auto take = [](int t, int s, int bpi, long frames) {
		AlsSessionClip c;
		c.track = t; c.scene = s;
		c.name = "take t" + std::to_string(t) + "s" + std::to_string(s);
		c.relPath = "looper/t" + std::to_string(t) + "_s" + std::to_string(s) + ".ogg";
		c.absPath = "/jam/" + c.relPath;
		c.frames = frames;
		c.sampleRate = 48000;
		c.bpm = 88.5;
		c.bpi = bpi;
		c.fileSize = 12345;
		c.mtime = 1756100000;
		return c;
	};
	p.sessionClips.push_back(take(0, 0, 16, 520339));
	p.sessionClips.push_back(take(0, 1, 16, 520339));
	p.sessionClips.push_back(take(3, 7, 8, 260169));
	p.sessionClips.push_back(take(7, 0, 16, 520339));

	AlsArrangementTrack tx;
	tx.isTx = true;
	tx.name = "you (tx)";
	AlsArrangementTrack player;
	player.name = "alice ch1";
	for (int i = 0; i < 3; i++) {
		AlsArrangementClip c;
		c.name = "iv " + std::to_string(i);
		c.relPath = "players/alice_" + std::to_string(i) + ".ogg";
		c.absPath = "/jam/" + c.relPath;
		c.sessionFrame = (uint64_t) (2 - i) * 520339;   // deliberately unsorted
		c.frames = 520339;
		c.sampleRate = 48000;
		c.fileSize = 999;
		player.clips.push_back(c);
	}
	{
		AlsArrangementClip c;
		c.name = "tx 0";
		c.relPath = "tx/tx_0.ogg";
		c.absPath = "/jam/tx/tx_0.ogg";
		c.sessionFrame = 0;
		c.frames = 520339;
		c.sampleRate = 48000;
		c.fileSize = 999;
		tx.clips.push_back(c);
	}
	p.arrangementTracks.push_back(player);
	p.arrangementTracks.push_back(tx);

	std::string err;
	std::string xml = buildAlsXml(p, tpl, &err);
	CHECK(!xml.empty(), ("buildAlsXml succeeds" + (err.empty() ? "" : (" (err: " + err + ")"))).c_str());
	if (xml.empty()) return 1;

	std::printf("== template demo content fully cleared ==\n");
	CHECK(xml.find("Core Library") == std::string::npos, "no Core Library refs remain");
	CHECK(xml.find(".aif") == std::string::npos, "no .aif sample refs remain");
	CHECK(xml.find("Ambient Encounters") == std::string::npos, "no demo clip names remain");
	CHECK(xml.find("Ableton Live 11 Lite.app") == std::string::npos, "no install paths remain");
	CHECK(xml.find("/Users/") == std::string::npos
	      && xml.find("Application Support") == std::string::npos,
	      "no home-dir paths leak (return-track preset refs scrubbed)");

	std::printf("== tracks ==\n");
	CHECK(countOf(xml, "<AudioTrack Id=") == 10, "8 looper + 2 cloned arrangement tracks");
	CHECK(xml.find("<EffectiveName Value=\"bass &amp; keys\"") != std::string::npos,
	      "track 2 renamed (escaped)");
	CHECK(xml.find("<UserName Value=\"guitar &lt;lead&gt;\"") != std::string::npos,
	      "track 3 user name set (escaped)");
	CHECK(xml.find("<EffectiveName Value=\"alice ch1\"") != std::string::npos, "player track cloned");
	CHECK(xml.find("<EffectiveName Value=\"you (tx)\"") != std::string::npos, "tx track cloned");

	std::printf("== clips ==\n");
	CHECK(countOf(xml, "<AudioClip Id=") == 8, "4 session + 4 arrangement clips");
	// A filled Session slot is the one place a <Value> wraps an <AudioClip> directly
	// (device-chain <Value> wrappers hold preset refs, at the same tab depth).
	CHECK(countOf(xml, "<Value>\n\t\t\t\t\t\t\t\t\t\t<AudioClip ") == 4,
	      "exactly 4 filled session slots");
	CHECK(xml.find("looper/t3_s7.ogg") != std::string::npos, "take file referenced (relative)");
	// Privacy: absolute sample paths must never reach the set — Live resolves via
	// RelativePath + the project marker; an absolute path only leaks the account name.
	CHECK(xml.find("/jam/") == std::string::npos, "no absolute sample paths in the set");
	// The t3s7 take: 8 beats long, looping.
	size_t c37 = xml.find("<Name Value=\"take t3s7\"");
	CHECK(c37 != std::string::npos, "t3s7 clip present");
	if (c37 != std::string::npos) {
		size_t clipStart = xml.rfind("<AudioClip", c37);
		std::string clip = xml.substr(clipStart, xml.find("</AudioClip>", clipStart) - clipStart);
		CHECK(clip.find("<LoopEnd Value=\"8\"") != std::string::npos, "t3s7 loops 8 beats (bpi)");
		CHECK(clip.find("<LoopOn Value=\"true\"") != std::string::npos, "t3s7 loop enabled");
		CHECK(clip.find("<IsWarped Value=\"true\"") != std::string::npos, "t3s7 warped");
		CHECK(clip.find("BeatTime=\"8\"") != std::string::npos, "warp marker pins 8 beats");
		CHECK(clip.find("<DefaultDuration Value=\"260169\"") != std::string::npos, "frames recorded");
		CHECK(clip.find("<Type Value=\"2\"") != std::string::npos,
		      "FileRef Type=2 (audio file — what Live's own refs use)");
		CHECK(clip.find("<LastModDate Value=\"1756100000\"") != std::string::npos,
		      "LastModDate carries the OGG's mtime");
	}
	// t3s7 sits in track 4's slot 7 (its <ClipSlot Id="7">), not anywhere else: the
	// clip must appear between the drums track's name and the 5th track's.
	size_t drums = xml.find("<EffectiveName Value=\"drums\"");
	size_t five = xml.find("<EffectiveName Value=\"Track 5\"");
	CHECK(drums != std::string::npos && five != std::string::npos
	      && c37 > drums && c37 < five, "t3s7 clip is inside the drums track");

	std::printf("== arrangement ==\n");
	// The player's 3 clips must be time-sorted in <Events>: Time attrs ascending.
	size_t alice = xml.find("<EffectiveName Value=\"alice ch1\"");
	std::string aliceBlock = xml.substr(alice, xml.find("</AudioTrack>", alice) - alice);
	std::vector<long> times;
	size_t tp = 0;
	while ((tp = aliceBlock.find("<AudioClip Id=\"", tp)) != std::string::npos) {
		size_t ta = aliceBlock.find("Time=\"", tp);
		if (ta == std::string::npos) break;
		times.push_back(std::strtol(aliceBlock.c_str() + ta + 6, nullptr, 10));
		tp = ta;
	}
	bool sorted = times.size() == 3;
	for (size_t i = 1; i < times.size(); i++)
		if (times[i] < times[i - 1]) sorted = false;
	CHECK(sorted, "arrangement clips sorted by session time");
	// Clip ids inside one <Events> list must be unique (Live: "Non-unique list ids");
	// ours are sequential 0,1,2 like Live's own arrangement serialization.
	std::vector<long> evIds = idsOf(aliceBlock, "<AudioClip Id=\"");
	std::set<long> evSet(evIds.begin(), evIds.end());
	CHECK(evIds.size() == 3 && evSet.size() == 3 && evSet.count(0) && evSet.count(1) && evSet.count(2),
	      "arrangement clip ids sequential + unique within Events");
	// Position: sessionFrame 520339 @48k @88.5bpm = 16 beats.
	CHECK(aliceBlock.find("Time=\"15.9") != std::string::npos
	      || aliceBlock.find("Time=\"16\"") != std::string::npos,
	      "second interval lands near beat 16");

	std::printf("== id graph ==\n");
	// Every id in the document-global pointee space must be unique: <Pointee> and every
	// "...Target" element (AutomationTarget, ModulationTarget, VolumeModulationTarget,
	// GrainSize/Flux/SampleOffset/Transposition...). Live refuses the set with
	// "Non-unique list ids" otherwise — this is what the track clones must renumber.
	std::set<long> targets;
	bool dupTarget = false;
	{
		size_t sp = 0;
		while ((sp = xml.find('<', sp)) != std::string::npos) {
			size_t ne = sp + 1;
			while (ne < xml.size() && (std::isalnum((unsigned char) xml[ne]) || xml[ne] == '_')) ne++;
			std::string tag = xml.substr(sp + 1, ne - (sp + 1));
			bool pointeeSpace = tag == "Pointee"
			    || (tag.size() > 6 && tag.compare(tag.size() - 6, 6, "Target") == 0);
			if (pointeeSpace && xml.compare(ne, 5, " Id=\"") == 0) {
				long id = std::strtol(xml.c_str() + ne + 5, nullptr, 10);
				if (!targets.insert(id).second) dupTarget = true;
			}
			sp = ne;
		}
	}
	CHECK(!dupTarget, "no duplicate ids in the pointee space (Pointee + all *Target tags)");
	long maxTarget = 0;
	for (long id : targets) if (id > maxTarget) maxTarget = id;
	std::vector<long> np = idsOf(xml, "<NextPointeeId Value=\"");
	CHECK(np.size() == 1 && np[0] > maxTarget, "NextPointeeId above every target id");
	std::vector<long> warpIds = idsOf(xml, "<WarpMarker Id=\"");
	std::set<long> warpSet(warpIds.begin(), warpIds.end());
	CHECK(warpIds.size() == 16 && warpSet.size() == 16, "16 unique warp marker ids (2 per clip)");
	std::vector<long> trackIds = idsOf(xml, "<AudioTrack Id=\"");
	std::set<long> trackSet(trackIds.begin(), trackIds.end());
	CHECK(trackSet.size() == 10, "all track ids unique");

	std::printf("== globals ==\n");
	size_t master = xml.find("<MasterTrack>");
	size_t tempoAt = master == std::string::npos ? std::string::npos : xml.find("<Tempo>", master);
	CHECK(tempoAt != std::string::npos
	      && xml.find("<Manual Value=\"88.5\"", tempoAt) == xml.find("<Manual Value=\"", tempoAt),
	      "master tempo patched to 88.5");
	// Live derives the shown tempo from the master's tempo AUTOMATION envelope, not
	// Manual — the template's 109.81076 initial FloatEvent must be gone entirely.
	CHECK(xml.find("109.81") == std::string::npos,
	      "template tempo automation envelope repointed (no 109.81 anywhere)");
	{
		size_t master = xml.find("<MasterTrack>");
		std::string mblk = xml.substr(master, xml.find("</MasterTrack>", master) - master);
		CHECK(mblk.find("<FloatEvent Id=\"1\" Time=\"-63072000\" Value=\"88.5\"") != std::string::npos,
		      "tempo envelope's initial event carries the set tempo");
	}

	std::printf("== transport ==\n");
	{
		size_t tr = xml.find("<Transport>");
		std::string tb = xml.substr(tr, xml.find("</Transport>", tr) - tr);
		CHECK(tb.find("<CurrentTime Value=\"0\"") != std::string::npos, "playhead reset to bar 1");
		CHECK(tb.find("<LoopStart Value=\"0\"") != std::string::npos, "loop brace starts at bar 1");
		CHECK(tb.find("<LoopLength Value=\"8\"") == std::string::npos
		      && tb.find("<LoopLength Value=\"16\"") == std::string::npos,
		      "loop brace sized to the jam, not the donor's leftovers");
	}

	std::printf("== structure intact ==\n");
	// Each audio track has 2 ClipSlotLists (MainSequencer + FreezeSequencer), so 2
	// clones add 4 to the template's own count.
	CHECK(countOf(xml, "<ClipSlotList>") == countOf(tpl, "<ClipSlotList>") + 4
	      && countOf(xml, "</ClipSlotList>") == countOf(xml, "<ClipSlotList>"),
	      "clip slot lists balanced (8 tracks + 2 clones)");
	CHECK(countOf(xml, "<ReturnTrack Id=") == 2 && countOf(xml, "<MasterTrack>") == 1
	      && countOf(xml, "<PreHearTrack>") == 1, "returns/master/prehear untouched");
	CHECK(countOf(xml, "<Scene Id=") == 8, "8 scenes untouched");

	std::printf("== gzip container ==\n");
	std::vector<uint8_t> als = buildAls(p, tpl, &err);
	bool gzOk = false;
	std::string inflated = gunzipStored(als, &gzOk);
	CHECK(gzOk && inflated == xml, "gzip round-trips (stored blocks + ISIZE)");

	std::printf("== timeline rebase ==\n");
	{
		// With an origin of one interval, every arrangement position shifts left by 16
		// beats (the session clock runs from the JOIN; the export starts at bar 1).
		AlsProject pr = p;
		pr.timelineOrigin = 520339;
		std::string xr = buildAlsXml(pr, tpl, &err);
		CHECK(!xr.empty(), "rebased set builds");
		if (!xr.empty()) {
			size_t al = xr.find("<EffectiveName Value=\"alice ch1\"");
			std::string blk = xr.substr(al, xr.find("</AudioTrack>", al) - al);
			CHECK(blk.find("Time=\"31.9") == std::string::npos && blk.find("Time=\"32\"") == std::string::npos,
			      "no clip stays at the un-rebased position");
			CHECK(blk.find("Time=\"15.9") != std::string::npos || blk.find("Time=\"16\"") != std::string::npos,
			      "the last interval moved one interval left");
		}
	}

	std::printf("== looper lanes (as-played timeline) ==\n");
	{
		// Grid: 16 beats/interval at 88.5 bpm, 48k → N = 520339-ish; use exact frames.
		const long N48 = 520339;
		std::vector<LooperTakeIn> takes;
		LooperTakeIn k;
		k.track = 0; k.slot = 0; k.file = "t0_s0.ogg"; k.absPath = "/jam/looper/t0_s0.ogg";
		k.frames = N48; k.sampleRate = 48000; k.bpm = 88; k.bpi = 16; k.startFrame = 1000;
		k.fileSize = 111; k.mtime = 222;
		takes.push_back(k);
		k.slot = 1; k.file = "t0_s1.ogg"; k.absPath = "/jam/looper/t0_s1.ogg"; k.startFrame = 2000;
		takes.push_back(k);

		auto mkEv = [](bool start, int t, int s, uint64_t sf, uint64_t take, bool rest) {
			LoopEventIn e;
			e.start = start; e.track = t; e.slot = s; e.sessionFrame = sf;
			e.takeStartFrame = take; e.rest = rest; e.bpm = 88; e.bpi = 16; e.sampleRate = 48000;
			return e;
		};
		std::vector<LoopEventIn> evs;
		evs.push_back(mkEv(true, 0, 0, 0, 1000, false));          // s0 plays [0, 2N)
		evs.push_back(mkEv(false, 0, 0, 2 * N48, 1000, false));
		evs.push_back(mkEv(true, 0, 0, 3 * N48, 1000, false));    // s0 again [3N, ...
		evs.push_back(mkEv(true, 0, 1, 5 * N48, 2000, false));    // ...s1 START closes s0 at 5N (monophonic)
		// s1 never stops → closes at timelineEnd (8N)
		evs.push_back(mkEv(true, 1, 2, 0, 777, false));           // identity mismatch: no such take → skipped
		evs.push_back(mkEv(false, 1, 2, 2 * N48, 777, false));
		evs.push_back(mkEv(true, 2, 3, 0, 0, true));              // rest span → skipped
		evs.push_back(mkEv(false, 2, 3, N48, 0, true));
		evs.push_back(mkEv(true, 3, 4, N48, 1000, false));        // zero-length: START+STOP same sf
		evs.push_back(mkEv(false, 3, 4, N48, 1000, false));

		std::string laneNames[8] = {"gtr", "", "", "", "", "", "", ""};
		AlsProject lp;
		lp.tempo = 88.5;
		for (int t = 0; t < 8; t++) lp.looperTrackNames[t] = laneNames[t];
		buildLooperLanes(evs, takes, laneNames, 8 * N48, lp.tempo, lp);
		CHECK(lp.looperLanes[0].size() == 3, "three spans survive on track 1's own lane");
		bool othersEmpty = true;
		for (int t = 1; t < 8; t++)
			if (!lp.looperLanes[t].empty()) othersEmpty = false;
		CHECK(othersEmpty, "skipped/rest/mismatch tracks produce no lane clips");
		if (lp.looperLanes[0].size() == 3) {
			const std::vector<AlsArrangementClip>& lane = lp.looperLanes[0];
			CHECK(lane[0].sessionFrame == 0 && lane[0].endSessionFrame == (uint64_t) (2 * N48)
			      && lane[0].loop && lane[0].loopLenBeats == 16,
			      "span 1: [0, 2N) looping 16 beats");
			CHECK(lane[1].sessionFrame == (uint64_t) (3 * N48)
			      && lane[1].endSessionFrame == (uint64_t) (5 * N48),
			      "span 2 closed by the next START (monophonic)");
			CHECK(lane[2].sessionFrame == (uint64_t) (5 * N48)
			      && lane[2].endSessionFrame == (uint64_t) (8 * N48)
			      && lane[2].relPath == "looper/t0_s1.ogg",
			      "span 3: unpaired START closed at timeline end, right file");
		}
		// The bpm<=0 (simulated clock) fallback: loop length derives from set tempo.
		std::vector<LoopEventIn> evs0;
		LoopEventIn z = mkEv(true, 0, 0, 0, 1000, false);
		z.bpm = 0; z.bpi = 0;
		evs0.push_back(z);
		AlsProject lp0;
		lp0.tempo = 88.5;
		buildLooperLanes(evs0, takes, laneNames, 2 * N48, lp0.tempo, lp0);
		CHECK(lp0.looperLanes[0].size() == 1
		      && std::fabs(lp0.looperLanes[0][0].loopLenBeats
		                   - (double) N48 / 48000 * 88.5 / 60.0) < 0.01,
		      "bpm=0 events fall back to set-tempo beat math");

		// XML shape: as-played spans live on the grid track's OWN arrangement lane —
		// no cloned lane tracks.
		AlsProject px;
		px.title = "lanes";
		px.tempo = 88.5;
		for (int t = 0; t < 8; t++) px.looperTrackNames[t] = laneNames[t];
		buildLooperLanes(evs, takes, laneNames, 8 * N48, px.tempo, px);
		std::string xml2 = buildAlsXml(px, tpl, &err);
		CHECK(!xml2.empty(), "lanes-only set builds");
		if (!xml2.empty()) {
			CHECK(countOf(xml2, "<AudioTrack Id=") == 8, "no cloned tracks for looper lanes");
			size_t trk = xml2.find("<EffectiveName Value=\"gtr\"");
			std::string blk = xml2.substr(trk, xml2.find("</AudioTrack>", trk) - trk);
			size_t c1 = blk.find("<Name Value=\"gtr s1\"");
			CHECK(c1 != std::string::npos, "span clip sits inside its own track's lane");
			if (c1 != std::string::npos) {
				size_t cs = blk.rfind("<AudioClip", c1);
				std::string clip = blk.substr(cs, blk.find("</AudioClip>", cs) - cs);
				CHECK(clip.find("<LoopOn Value=\"true\"") != std::string::npos, "span clip loops");
				CHECK(clip.find("<LoopEnd Value=\"16\"") != std::string::npos, "loop cycle = 16 beats");
				double spanBeats = 2.0 * N48 / 48000 * 88.5 / 60.0;
				double gotEnd = 0;
				size_t ce = clip.find("<CurrentEnd Value=\"");
				if (ce != std::string::npos)
					gotEnd = std::atof(clip.c_str() + ce + std::strlen("<CurrentEnd Value=\""));
				CHECK(std::fabs(gotEnd - spanBeats) < 0.05, "clip extent covers the span");
			}
		}
	}

	std::printf("== Live Lite flavor (8-track cap) ==\n");
	{
		AlsProject lt;
		lt.title = "lite";
		lt.tempo = 80;
		lt.looperTracks = 6;
		lt.inlineArrangement = true;
		const char* nm[8] = {"a", "b", "c", "d", "e", "f", "", ""};
		for (int t = 0; t < 8; t++) lt.looperTrackNames[t] = nm[t];
		AlsSessionClip sc;
		sc.track = 6; sc.scene = 0; sc.name = "beyond"; // 7th track: must be dropped
		sc.relPath = "looper/t6_s0.ogg"; sc.absPath = "/jam/looper/t6_s0.ogg";
		sc.frames = 520339; sc.sampleRate = 48000; sc.bpm = 80; sc.bpi = 16;
		lt.sessionClips.push_back(sc);
		sc.track = 0; sc.name = "kept"; sc.relPath = "looper/t0_s0.ogg";
		lt.sessionClips.push_back(sc);
		AlsArrangementTrack pl2;
		pl2.name = "players";
		AlsArrangementClip ac;
		ac.name = "players 0"; ac.relPath = "players/000000_x.ogg";
		ac.sessionFrame = 0; ac.frames = 520339; ac.sampleRate = 48000;
		pl2.clips.push_back(ac);
		AlsArrangementTrack tx2;
		tx2.isTx = true; tx2.name = "you (tx)";
		ac.name = "tx 1"; ac.relPath = "tx/000001_mix.ogg"; ac.sessionFrame = 520339;
		tx2.clips.push_back(ac);
		lt.arrangementTracks.push_back(pl2);
		lt.arrangementTracks.push_back(tx2);
		std::string xl = buildAlsXml(lt, tpl, &err);
		CHECK(!xl.empty(), "Lite set builds");
		if (!xl.empty()) {
			CHECK(countOf(xl, "<AudioTrack Id=") == 8, "Lite: exactly 8 audio tracks, no clones");
			CHECK(xl.find("<Name Value=\"beyond\"") == std::string::npos,
			      "Lite: 7th-track take dropped from the grid");
			CHECK(xl.find("<Name Value=\"kept\"") != std::string::npos, "Lite: in-range take kept");
			CHECK(xl.find("<EffectiveName Value=\"players\"") != std::string::npos,
			      "Lite: template track 7 renamed to the merged players lane");
			CHECK(xl.find("<EffectiveName Value=\"you (tx)\"") != std::string::npos,
			      "Lite: template track 8 is the TX lane");
			size_t p7 = xl.find("<EffectiveName Value=\"players\"");
			std::string b7 = xl.substr(p7, xl.find("</AudioTrack>", p7) - p7);
			CHECK(b7.find("players/000000_x.ogg") != std::string::npos,
			      "Lite: player clip inside the inline track");
			// Over-budget arrangement is refused, not silently truncated.
			AlsProject bad = lt;
			bad.arrangementTracks.push_back(pl2); // a third lane can't fit 8-6
			std::string e3;
			CHECK(buildAlsXml(bad, tpl, &e3).empty() && !e3.empty(),
			      "Lite: >2 arrangement lanes refused with a reason");
		}
	}

	std::printf("== error paths ==\n");
	std::string e2;
	CHECK(buildAlsXml(p, "not xml", &e2).empty() && !e2.empty(), "bogus template refused with reason");

	// Leave the artifacts for manual inspection (xmllint / open in Live).
	::mkdir("build", 0755);
	::mkdir(outDir.c_str(), 0755);
	{
		std::ofstream f(outDir + "/" + p.title + ".xml", std::ios::binary | std::ios::trunc);
		f.write(xml.data(), (std::streamsize) xml.size());
	}
	{
		std::ofstream f(outDir + "/" + p.title + ".als", std::ios::binary | std::ios::trunc);
		f.write((const char*) als.data(), (std::streamsize) als.size());
	}
	std::printf("artifacts: %s/%s.{xml,als}\n", outDir.c_str(), p.title.c_str());

	if (failures) { std::printf("\n%d FAILURE(S)\n", failures); return 1; }
	std::printf("\nall checks passed\n");
	return 0;
}
