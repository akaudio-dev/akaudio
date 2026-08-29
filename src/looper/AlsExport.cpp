// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "AlsExport.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace akaudio {
namespace looper {

// ---- gzip via stored (uncompressed) DEFLATE blocks ------------------------------
// A valid .gz stream Live inflates, with no zlib dependency: 10-byte gzip header, then
// DEFLATE stored blocks (BTYPE=00: [BFINAL bit][LEN][NLEN][raw]), CRC32 + ISIZE trailer.
// The .als XML is small, so skipping compression is fine.

// A take's OWN length in beats — the warp/loop span the .als pins over the file.
// Since the fluid-jamming rework takes are any whole-beat length: `bpi` is only the
// INTERVAL's beat count (the cap), never assumed to be the take's. Derive from the
// audio itself and snap near-integers (the beat grid is integer-ceil spaced, so a
// b-beat take is within a few frames of exactly b beats); `bpi` is the last-resort
// fallback when the take carries no usable tempo.
static double takeLenBeats(long frames, double sampleRate, double bpm, int bpi) {
	if (frames > 0 && sampleRate > 0 && bpm > 0) {
		double beats = (double) frames / sampleRate * bpm / 60.0;
		double r = std::floor(beats + 0.5);
		// Beat-quantized capture lands within a couple of frames of a whole beat
		// (~0.0002 beats) — snap only that, never audio genuinely off the beat grid.
		if (r >= 1.0 && std::fabs(beats - r) < 0.005)
			return r;
		return beats > 0 ? beats : 4.0;
	}
	return bpi > 0 ? (double) bpi : 4.0;
}

static uint32_t crc32Of(const uint8_t* p, size_t n) {
	static uint32_t table[256];
	static bool init = false;
	if (!init) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		init = true;
	}
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; i++)
		c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

std::vector<uint8_t> gzipStore(const std::string& data) {
	const uint8_t* src = (const uint8_t*) data.data();
	const size_t n = data.size();
	std::vector<uint8_t> out;
	out.reserve(n + n / 65535 + 64);
	// gzip header: magic, CM=deflate, no flags, mtime 0, XFL 0, OS 255 (unknown).
	const uint8_t hdr[10] = {0x1f, 0x8b, 0x08, 0, 0, 0, 0, 0, 0, 0xff};
	out.insert(out.end(), hdr, hdr + 10);
	size_t pos = 0;
	if (n == 0) {
		out.push_back(0x01);                      // BFINAL=1, BTYPE=00
		out.push_back(0); out.push_back(0);       // LEN = 0
		out.push_back(0xff); out.push_back(0xff); // NLEN
	}
	while (pos < n) {
		size_t chunk = n - pos;
		if (chunk > 65535) chunk = 65535;
		bool last = (pos + chunk) >= n;
		out.push_back(last ? 0x01 : 0x00);        // BFINAL, BTYPE=00
		uint16_t len = (uint16_t) chunk, nlen = (uint16_t) ~len;
		out.push_back(len & 0xff); out.push_back((len >> 8) & 0xff);
		out.push_back(nlen & 0xff); out.push_back((nlen >> 8) & 0xff);
		out.insert(out.end(), src + pos, src + pos + chunk);
		pos += chunk;
	}
	uint32_t crc = crc32Of(src, n), isize = (uint32_t) n;
	for (int i = 0; i < 4; i++) out.push_back((crc >> (8 * i)) & 0xff);
	for (int i = 0; i < 4; i++) out.push_back((isize >> (8 * i)) & 0xff);
	return out;
}

// ---- small helpers ---------------------------------------------------------------

namespace {

std::string esc(const std::string& s) {
	std::string o;
	o.reserve(s.size() + 8);
	for (char c : s) {
		switch (c) {
			case '&':  o += "&amp;";  break;
			case '<':  o += "&lt;";   break;
			case '>':  o += "&gt;";   break;
			case '"':  o += "&quot;"; break;
			case '\'': o += "&apos;"; break;
			default:   o += c;
		}
	}
	return o;
}

std::string num(double v) {
	char b[64];
	(void) std::snprintf(b, sizeof(b), "%.9g", v);
	return b;
}

// The error sink: remembers the first failure so surgery can bail early.
struct Err {
	std::string msg;
	bool fail(const std::string& m) {
		if (msg.empty()) msg = m;
		return false;
	}
	bool ok() const { return msg.empty(); }
};

// Position of the matching `</tag>` for the `<tag ...>` opening at `openPos`, counting
// nested same-name tags (Live nests <ClipSlot> inside <ClipSlot Id="..">). Token-exact:
// `<tag` must be followed by '>' or ' ' so `<ClipSlot` never matches `<ClipSlotList>`.
// npos on no match. Self-closing same-name tags don't occur in the spans we walk.
size_t matchClose(const std::string& s, size_t openPos, const char* tag) {
	const std::string open = std::string("<") + tag;
	const std::string close = std::string("</") + tag + ">";
	int depth = 0;
	size_t i = openPos;
	while (true) {
		size_t o = s.find(open, i);
		size_t c = s.find(close, i);
		// Skip opens that are a longer tag name (e.g. <ClipSlotList> when tag=ClipSlot).
		while (o != std::string::npos && o + open.size() < s.size()
		       && s[o + open.size()] != '>' && s[o + open.size()] != ' ') {
			o = s.find(open, o + 1);
		}
		if (c == std::string::npos) return std::string::npos;
		if (o != std::string::npos && o < c) {
			depth++;
			i = o + open.size();
		} else {
			depth--;
			if (depth == 0) return c;
			i = c + close.size();
		}
	}
}

// Replace the value of the first `<tag Value="...">` at/after `from` inside `s`.
bool setValue(std::string& s, size_t from, const char* tag, const std::string& value, Err& err) {
	const std::string pat = std::string("<") + tag + " Value=\"";
	size_t p = s.find(pat, from);
	if (p == std::string::npos)
		return err.fail(std::string("template: no <") + tag + " Value=...>");
	size_t vs = p + pat.size();
	size_t ve = s.find('"', vs);
	if (ve == std::string::npos)
		return err.fail(std::string("template: unterminated <") + tag + ">");
	s.replace(vs, ve - vs, value);
	return true;
}

// ---- clip XML --------------------------------------------------------------------

// Emit one <AudioClip> block, modeled field-for-field on the template's own clips
// (Live 11.2 schema — see refs/Live11 Project). `depth` = tab depth of the <AudioClip>
// line (10 in a Session slot, 9 in the Arrangement Events). Warped, so the clip is
// tempo-locked: two warp markers pin `lenBeats` beats to the file's real duration.
// `warpId` hands out file-unique WarpMarker ids.
struct ClipSpec {
	std::string name;
	int clipId = 0;         // unique within the parent list: always 0 in a Session slot
	                        // (one clip per <Value>), sequential in Arrangement <Events>
	int color = 20;
	double startBeats = 0;  // Arrangement position; 0 in a Session slot
	double lenBeats = 4;    // loop cycle length (Loop/OutMarker/warp span)
	double spanBeats = 0;   // clip extent on the timeline; 0 ⇒ = lenBeats (one cycle)
	bool loop = false;      // true for Looper takes/spans, false for one-shot intervals
	std::string absPath, relPath;
	long frames = 0;
	double sampleRate = 48000;
	long fileSize = 0;
	long mtime = 0;
};

std::string audioClipXml(const ClipSpec& c, int depth, int& warpId) {
	std::string t0(depth, '\t');
	std::string o;
	o.reserve(6144);
	auto line = [&](int extra, const std::string& text) {
		o += t0;
		o.append((size_t) extra, '\t');
		o += text;
		o += '\n';
	};
	auto v = [&](int extra, const char* tag, const std::string& value) {
		line(extra, std::string("<") + tag + " Value=\"" + value + "\" />");
	};
	const double endBeats = c.startBeats + (c.spanBeats > 0 ? c.spanBeats : c.lenBeats);
	const double lenSec = c.sampleRate > 0 ? (double) c.frames / c.sampleRate : 0.0;

	line(0, "<AudioClip Id=\"" + std::to_string(c.clipId) + "\" Time=\"" + num(c.startBeats) + "\">");
	v(1, "LomId", "0");
	v(1, "LomIdView", "0");
	v(1, "CurrentStart", num(c.startBeats));
	v(1, "CurrentEnd", num(endBeats));
	line(1, "<Loop>");
	v(2, "LoopStart", "0");
	v(2, "LoopEnd", num(c.lenBeats));
	v(2, "StartRelative", "0");
	v(2, "LoopOn", c.loop ? "true" : "false");
	v(2, "OutMarker", num(c.lenBeats));
	v(2, "HiddenLoopStart", "0");
	v(2, "HiddenLoopEnd", num(c.lenBeats));
	line(1, "</Loop>");
	v(1, "Name", esc(c.name));
	v(1, "Annotation", "");
	v(1, "Color", std::to_string(c.color));
	v(1, "LaunchMode", "0");
	v(1, "LaunchQuantisation", "0");
	line(1, "<TimeSignature>");
	line(2, "<TimeSignatures>");
	line(3, "<RemoteableTimeSignature Id=\"0\">");
	v(4, "Numerator", "4");
	v(4, "Denominator", "4");
	v(4, "Time", "0");
	line(3, "</RemoteableTimeSignature>");
	line(2, "</TimeSignatures>");
	line(1, "</TimeSignature>");
	line(1, "<Envelopes>");
	line(2, "<Envelopes />");
	line(1, "</Envelopes>");
	line(1, "<ScrollerTimePreserver>");
	v(2, "LeftTime", "0");
	v(2, "RightTime", "0");
	line(1, "</ScrollerTimePreserver>");
	line(1, "<TimeSelection>");
	v(2, "AnchorTime", "0");
	v(2, "OtherTime", "0");
	line(1, "</TimeSelection>");
	v(1, "Legato", "false");
	v(1, "Ram", "false");
	line(1, "<GrooveSettings>");
	v(2, "GrooveId", "-1");
	line(1, "</GrooveSettings>");
	v(1, "Disabled", "false");
	v(1, "VelocityAmount", "0");
	line(1, "<FollowAction>");
	v(2, "FollowTime", "4");
	v(2, "IsLinked", "true");
	v(2, "LoopIterations", "1");
	v(2, "FollowActionA", "4");
	v(2, "FollowActionB", "0");
	v(2, "FollowChanceA", "100");
	v(2, "FollowChanceB", "0");
	v(2, "JumpIndexA", "1");
	v(2, "JumpIndexB", "1");
	v(2, "FollowActionEnabled", "false");
	line(1, "</FollowAction>");
	line(1, "<Grid>");
	v(2, "FixedNumerator", "1");
	v(2, "FixedDenominator", "16");
	v(2, "GridIntervalPixel", "20");
	v(2, "Ntoles", "2");
	v(2, "SnapToGrid", "true");
	v(2, "Fixed", "false");
	line(1, "</Grid>");
	v(1, "FreezeStart", "0");
	v(1, "FreezeEnd", "0");
	v(1, "IsWarped", "true");
	v(1, "TakeId", "-1");
	line(1, "<SampleRef>");
	line(2, "<FileRef>");
	v(3, "RelativePathType", "3");   // relative to the folder holding the .als
	v(3, "RelativePath", esc(c.relPath));
	// Deliberately blank: Live resolves via RelativePath + the Ableton Project Info
	// marker (proven — a VALID absolute path here was ignored while relative
	// resolution failed), and an absolute path embeds the account name in a file made
	// for sharing (CLAUDE.md's never-persist-absolute-paths rule).
	v(3, "Path", "");
	v(3, "Type", "2");               // 2 = audio file — every audio FileRef Live writes
	v(3, "LivePackName", "");
	v(3, "LivePackId", "");
	v(3, "OriginalFileSize", std::to_string(c.fileSize));
	v(3, "OriginalCrc", "0");
	line(2, "</FileRef>");
	v(2, "LastModDate", std::to_string(c.mtime));
	line(2, "<SourceContext />");
	v(2, "SampleUsageHint", "0");
	v(2, "DefaultDuration", std::to_string(c.frames));
	v(2, "DefaultSampleRate", num(c.sampleRate));
	line(1, "</SampleRef>");
	line(1, "<Onsets>");
	line(2, "<UserOnsets />");
	v(2, "HasUserOnsets", "false");
	line(1, "</Onsets>");
	v(1, "WarpMode", "0");           // Beats
	v(1, "GranularityTones", "30");
	v(1, "GranularityTexture", "65");
	v(1, "FluctuationTexture", "25");
	v(1, "TransientResolution", "6");
	v(1, "TransientLoopMode", "2");
	v(1, "TransientEnvelope", "100");
	v(1, "ComplexProFormants", "100");
	v(1, "ComplexProEnvelope", "128");
	v(1, "Sync", "true");
	v(1, "HiQ", "true");
	v(1, "Fade", "true");
	line(1, "<Fades>");
	v(2, "FadeInLength", "0");
	v(2, "FadeOutLength", "0");
	v(2, "ClipFadesAreInitialized", "true");
	v(2, "CrossfadeInState", "0");
	v(2, "FadeInCurveSkew", "0");
	v(2, "FadeInCurveSlope", "0");
	v(2, "FadeOutCurveSkew", "0");
	v(2, "FadeOutCurveSlope", "0");
	v(2, "IsDefaultFadeIn", "true");
	v(2, "IsDefaultFadeOut", "true");
	line(1, "</Fades>");
	v(1, "PitchCoarse", "0");
	v(1, "PitchFine", "0");
	v(1, "SampleVolume", "1");
	v(1, "MarkerDensity", "2");
	v(1, "AutoWarpTolerance", "4");
	line(1, "<WarpMarkers>");
	line(2, "<WarpMarker Id=\"" + std::to_string(warpId++) + "\" SecTime=\"0\" BeatTime=\"0\" />");
	line(2, "<WarpMarker Id=\"" + std::to_string(warpId++) + "\" SecTime=\"" + num(lenSec)
	        + "\" BeatTime=\"" + num(c.lenBeats) + "\" />");
	line(1, "</WarpMarkers>");
	line(1, "<SavedWarpMarkersForStretched />");
	v(1, "MarkersGenerated", "false");
	v(1, "IsSongTempoMaster", "false");
	line(0, "</AudioClip>");
	return o;
}

// ---- track surgery ---------------------------------------------------------------

// One template <AudioTrack> block, extracted for editing and reassembly.
struct TrackBlock {
	std::string xml;
	int color = 20;         // the track's own color, inherited by its clips
};

// Rename a track. Live derives EffectiveName from UserName for named tracks; set all
// three name fields so browsers/headers agree.
bool renameTrack(TrackBlock& tb, const std::string& name, Err& err) {
	return setValue(tb.xml, 0, "EffectiveName", esc(name), err)
	    && setValue(tb.xml, 0, "UserName", esc(name), err)
	    && setValue(tb.xml, 0, "MemorizedFirstClipName", "", err);
}

// Clear the track's 8 Session slots and its Arrangement events, dropping the template's
// demo clips (Core Library references must never leak into an export). Records the
// track color on the way.
bool clearTrack(TrackBlock& tb, Err& err) {
	std::string& s = tb.xml;
	{	// Track color: the <Color> right after the <Name> block.
		size_t nameEnd = s.find("</Name>");
		size_t p = nameEnd == std::string::npos ? std::string::npos : s.find("<Color Value=\"", nameEnd);
		if (p == std::string::npos) return err.fail("template: track without <Color>");
		tb.color = (int) std::strtol(s.c_str() + p + 14, nullptr, 10);
	}
	// Session slots: inside <ClipSlotList>, each <ClipSlot Id="n"> wraps an inner
	// <ClipSlot> holding <Value /> (empty) or <Value>...clip...</Value>.
	size_t listPos = s.find("<ClipSlotList>");
	if (listPos == std::string::npos) return err.fail("template: track without <ClipSlotList>");
	size_t listEnd = s.find("</ClipSlotList>", listPos);
	if (listEnd == std::string::npos) return err.fail("template: unterminated <ClipSlotList>");
	int slots = 0;
	size_t p = listPos;
	while (true) {
		size_t slot = s.find("<ClipSlot Id=\"", p);
		if (slot == std::string::npos || slot > listEnd) break;
		size_t vEmpty = s.find("<Value />", slot);
		size_t vFull = s.find("<Value>", slot);
		if (vFull != std::string::npos && (vEmpty == std::string::npos || vFull < vEmpty)) {
			// Filled slot: drop the whole clip, leave an empty <Value />.
			size_t vClose = matchClose(s, vFull, "Value");
			if (vClose == std::string::npos) return err.fail("template: unterminated slot <Value>");
			size_t n = vClose + std::strlen("</Value>") - vFull;
			s.replace(vFull, n, "<Value />");
			listEnd = s.find("</ClipSlotList>", slot); // s shifted
		}
		slots++;
		p = slot + 1;
	}
	if (slots != 8) return err.fail("template: expected 8 clip slots per track");
	// Arrangement: <Sample><ArrangerAutomation><Events> holds the track's timeline clips.
	size_t arr = s.find("<ArrangerAutomation>");
	if (arr == std::string::npos) return err.fail("template: track without <ArrangerAutomation>");
	size_t ev = s.find("<Events", arr);
	if (ev == std::string::npos) return err.fail("template: track without <Events>");
	// Normalize either form (filled <Events>...</Events> or already-empty) to <Events />.
	if (s.compare(ev, std::strlen("<Events>"), "<Events>") == 0) {
		size_t evClose = matchClose(s, ev, "Events");
		if (evClose == std::string::npos) return err.fail("template: unterminated <Events>");
		s.replace(ev, evClose + std::strlen("</Events>") - ev, "<Events />");
	}
	return true;
}

// Insert `clipXml` into Session slot `scene` (0..7) of a cleared track: the slot's
// `<Value />` becomes `<Value>\n...\n</Value>`.
bool fillSlot(TrackBlock& tb, int scene, const std::string& clipXml, Err& err) {
	std::string& s = tb.xml;
	const std::string anchor = "<ClipSlot Id=\"" + std::to_string(scene) + "\">";
	size_t listPos = s.find("<ClipSlotList>");
	size_t slot = listPos == std::string::npos ? std::string::npos : s.find(anchor, listPos);
	if (slot == std::string::npos)
		return err.fail("template: no clip slot " + std::to_string(scene));
	// Bound the search by this slot's own extent: without it, a duplicate manifest row
	// for an already-filled slot would find the NEXT slot's <Value /> and silently
	// plant the clip in the wrong scene.
	size_t slotEnd = matchClose(s, slot, "ClipSlot");
	size_t v = s.find("<Value />", slot);
	if (v == std::string::npos || (slotEnd != std::string::npos && v > slotEnd))
		return err.fail("slot " + std::to_string(scene) + " not empty");
	// Indentation: the <Value> line sits at 9 tabs, the clip at 10 (Live 11 layout).
	std::string filled = "<Value>\n" + clipXml + std::string(9, '\t') + "</Value>";
	s.replace(v, std::strlen("<Value />"), filled);
	return true;
}

// Fill a cleared track's Arrangement <Events /> with clips (already concatenated XML).
bool fillEvents(TrackBlock& tb, const std::string& clipsXml, Err& err) {
	std::string& s = tb.xml;
	size_t arr = s.find("<ArrangerAutomation>");
	size_t ev = arr == std::string::npos ? std::string::npos : s.find("<Events />", arr);
	if (ev == std::string::npos) return err.fail("template: no empty <Events /> to fill");
	std::string filled = "<Events>\n" + clipsXml + std::string(8, '\t') + "</Events>";
	s.replace(ev, std::strlen("<Events />"), filled);
	return true;
}

// Renumber a cloned track so its ids can't collide with the originals'. The
// <AudioTrack Id> gets `trackId`, and every id in the document-global pointee space —
// <Pointee> and every "...Target" element (AutomationTarget, ModulationTarget,
// VolumeModulationTarget, TranspositionModulationTarget, GrainSize/Flux/SampleOffset
// ModulationTarget, …) — is reallocated from `nextPointee`; the template gives each
// track distinct values for all of these, so a clone must too. Ids scoped to a parent
// list (ClipSlot 0-7, TrackSendHolder, AutomationLane) legitimately repeat per track
// and stay. Everything else in the block is Live's own serialization, untouched.
bool renumberClone(TrackBlock& tb, int trackId, long& nextPointee, Err& err) {
	std::string& s = tb.xml;
	const std::string open = "<AudioTrack Id=\"";
	size_t p = s.find(open);
	if (p == std::string::npos) return err.fail("clone: no <AudioTrack Id>");
	size_t vs = p + open.size(), ve = s.find('"', vs);
	if (ve == std::string::npos) return err.fail("clone: bad <AudioTrack Id>");
	s.replace(vs, ve - vs, std::to_string(trackId));
	const std::string idAttr = " Id=\"";
	size_t at = 0;
	while ((at = s.find('<', at)) != std::string::npos) {
		size_t nameEnd = at + 1;
		while (nameEnd < s.size() && (std::isalnum((unsigned char) s[nameEnd]) || s[nameEnd] == '_'))
			nameEnd++;
		std::string tag = s.substr(at + 1, nameEnd - (at + 1));
		bool pointeeSpace = tag == "Pointee"
		    || (tag.size() > 6 && tag.compare(tag.size() - 6, 6, "Target") == 0);
		if (pointeeSpace && s.compare(nameEnd, idAttr.size(), idAttr) == 0) {
			size_t is = nameEnd + idAttr.size(), ie = s.find('"', is);
			if (ie == std::string::npos) return err.fail("clone: bad " + tag + " id");
			s.replace(is, ie - is, std::to_string(nextPointee++));
		}
		at = nameEnd;
	}
	return true;
}

// Blank every preset-provenance path in a template span (the tail: return / master /
// prehear device chains). The template's return-track devices carry `.adv` preset
// FileRefs with the absolute path of whoever saved the set — a home-dir (account name)
// leak, same rule as Radio's "never persist an absolute favicon path". Live treats
// these as informational (where the preset came from); blanking them keeps the device
// and all its parameter values.
void scrubPresetRefs(std::string& s) {
	for (const char* tag : {"RelativePath", "Path", "BrowserContentPath",
	                        "LivePackName", "LivePackId"}) {
		const std::string pat = std::string("<") + tag + " Value=\"";
		size_t p = 0;
		while ((p = s.find(pat, p)) != std::string::npos) {
			size_t vs = p + pat.size();
			size_t ve = s.find('"', vs);
			if (ve == std::string::npos) return;
			s.erase(vs, ve - vs);
			p = vs;
		}
	}
}

} // namespace

// ---- the build -------------------------------------------------------------------

std::string buildAlsXml(const AlsProject& p, const std::string& tpl, std::string* errOut) {
	Err err;
	std::string xml;
	do {
		if (tpl.find("<Ableton ") == std::string::npos) {
			err.fail("template: not an Ableton Live set XML");
			break;
		}

		// Carve the document into [head][track 0..7][tail] on the <AudioTrack> blocks.
		TrackBlock tracks[8];
		size_t cur = 0;
		size_t firstTrack = std::string::npos, lastTrackEnd = std::string::npos;
		bool carved = true;
		for (int t = 0; t < 8; t++) {
			size_t open = tpl.find("<AudioTrack Id=\"", cur);
			if (open == std::string::npos) {
				carved = !err.fail("template: expected 8 <AudioTrack> blocks");
				break;
			}
			size_t close = tpl.find("</AudioTrack>", open);
			if (close == std::string::npos) {
				carved = !err.fail("template: unterminated <AudioTrack>");
				break;
			}
			close += std::strlen("</AudioTrack>");
			if (t == 0) firstTrack = open;
			tracks[t].xml = tpl.substr(open, close - open);
			lastTrackEnd = close;
			cur = close;
		}
		if (!carved) break;
		if (tpl.find("<AudioTrack Id=\"", cur) != std::string::npos) {
			err.fail("template: more than 8 <AudioTrack> blocks");
			break;
		}
		std::string head = tpl.substr(0, firstTrack);
		std::string tail = tpl.substr(lastTrackEnd);
		scrubPresetRefs(tail);
		// The inter-track whitespace: tracks sit at tab depth 3 (Ableton>LiveSet>Tracks).
		const std::string sep = "\n\t\t\t";

		// Reset every track to an empty, renamed shell; keep a cleared copy as the
		// donor for Arrangement clones (made before slots are filled).
		for (int t = 0; t < 8; t++)
			if (!clearTrack(tracks[t], err)) break;
		if (!err.ok()) break;
		TrackBlock donor = tracks[7];
		for (int t = 0; t < 8; t++) {
			std::string name = !p.looperTrackNames[t].empty()
			                   ? p.looperTrackNames[t] : ("Track " + std::to_string(t + 1));
			if (!renameTrack(tracks[t], name, err)) break;
		}
		if (!err.ok()) break;

		// Our takes into their Session slots. Warp marker ids restart well above the
		// template's own (single digits); they only need to be unique within the file.
		int warpId = 2000;
		bool filled = true;
		for (const AlsSessionClip& c : p.sessionClips) {
			if (c.track < 0 || c.track >= p.looperTracks || c.track >= 8
			        || c.scene < 0 || c.scene >= 8) continue;
			ClipSpec spec;
			spec.name = c.name;
			spec.color = tracks[c.track].color;
			spec.startBeats = 0;
			spec.lenBeats = takeLenBeats(c.frames, c.sampleRate, c.bpm, c.bpi);
			spec.loop = true;
			spec.absPath = c.absPath;
			spec.relPath = c.relPath;
			spec.frames = c.frames;
			spec.sampleRate = c.sampleRate;
			spec.fileSize = c.fileSize;
			spec.mtime = c.mtime;
			filled = fillSlot(tracks[c.track], c.scene, audioClipXml(spec, 10, warpId), err);
			if (!filled) break;
		}
		if (!filled) break;

		// Arrangement clip emission, shared by every lane flavor: the grid tracks' own
		// as-played lanes, inline (Lite) tracks, and cloned tracks. Tracks the overall
		// arrangement extent for the transport loop brace below.
		double maxEndBeats = 0;
		auto emitLane = [&](const std::vector<AlsArrangementClip>& clips, int color) {
			std::string clipsXml;
			std::vector<const AlsArrangementClip*> sorted;
			sorted.reserve(clips.size());
			for (const AlsArrangementClip& c : clips) sorted.push_back(&c);
			std::stable_sort(sorted.begin(), sorted.end(),
				[](const AlsArrangementClip* a, const AlsArrangementClip* b) {
					return a->sessionFrame < b->sessionFrame;
				});
			int clipId = 0;
			for (const AlsArrangementClip* c : sorted) {
				ClipSpec spec;
				spec.name = c->name;
				spec.clipId = clipId++;   // unique within this track's <Events> list
				spec.color = color;
				// Timeline rebase: positions are relative to the project origin.
				uint64_t sf = c->sessionFrame > p.timelineOrigin
				              ? c->sessionFrame - p.timelineOrigin : 0;
				spec.startBeats = c->sampleRate > 0
				                  ? (double) sf / c->sampleRate * p.tempo / 60.0 : 0.0;
				if (c->loop && c->endSessionFrame > c->sessionFrame) {
					// A Looper play-span: the take loops inside the span's extent.
					spec.lenBeats = c->loopLenBeats > 0 ? c->loopLenBeats
					                : (c->sampleRate > 0
					                   ? (double) c->frames / c->sampleRate * p.tempo / 60.0 : 4.0);
					spec.spanBeats = c->sampleRate > 0
					                 ? (double) (c->endSessionFrame - c->sessionFrame)
					                   / c->sampleRate * p.tempo / 60.0 : spec.lenBeats;
					spec.loop = true;
				} else {
					spec.lenBeats = c->sampleRate > 0
					                ? (double) c->frames / c->sampleRate * p.tempo / 60.0 : 4.0;
					spec.loop = false;
				}
				spec.absPath = c->absPath;
				spec.relPath = c->relPath;
				spec.frames = c->frames;
				spec.sampleRate = c->sampleRate;
				spec.fileSize = c->fileSize;
				spec.mtime = c->mtime;
				double endB = spec.startBeats + (spec.spanBeats > 0 ? spec.spanBeats : spec.lenBeats);
				if (endB > maxEndBeats) maxEndBeats = endB;
				clipsXml += audioClipXml(spec, 9, warpId);
			}
			return clipsXml;
		};

		// The grid tracks' own as-played lanes (their Arrangement side, cleared above).
		bool lanesOk = true;
		for (int t = 0; t < p.looperTracks && t < 8; t++) {
			if (p.looperLanes[t].empty()) continue;
			std::string clipsXml = emitLane(p.looperLanes[t], tracks[t].color);
			if (!clipsXml.empty() && !fillEvents(tracks[t], clipsXml, err)) {
				lanesOk = false;
				break;
			}
		}
		if (!lanesOk) break;

		long nextPointee = 0;
		{
			const std::string np = "<NextPointeeId Value=\"";
			size_t q = tpl.find(np);
			if (q == std::string::npos) {
				err.fail("template: no <NextPointeeId>");
				break;
			}
			nextPointee = std::strtol(tpl.c_str() + q + np.size(), nullptr, 10);
		}
		std::string clones;
		bool arrOk = true;
		if (p.inlineArrangement) {
			// Live Lite flavor: the arrangement lands in the template tracks after the
			// grid (renamed) — its 8-track cap forbids clones.
			if ((int) p.arrangementTracks.size() > 8 - p.looperTracks) {
				err.fail("Lite flavor: arrangement needs more tracks than the 8-track cap allows");
				break;
			}
			for (size_t i = 0; i < p.arrangementTracks.size(); i++) {
				TrackBlock& tb = tracks[p.looperTracks + (int) i];
				const AlsArrangementTrack& at = p.arrangementTracks[i];
				if (!renameTrack(tb, at.name, err)) { arrOk = false; break; }
				std::string clipsXml = emitLane(at.clips, tb.color);
				if (!clipsXml.empty() && !fillEvents(tb, clipsXml, err)) { arrOk = false; break; }
			}
		} else {
			// Standard flavor: one cloned track per player + TX, pointee ids renumbered
			// from the set's own NextPointeeId.
			int nextTrackId = 1000;  // far above the template's own track ids (2..20)
			for (const AlsArrangementTrack& at : p.arrangementTracks) {
				TrackBlock tb = donor;
				if (!renumberClone(tb, nextTrackId++, nextPointee, err)
				    || !renameTrack(tb, at.name, err)) {
					arrOk = false;
					break;
				}
				std::string clipsXml = emitLane(at.clips, tb.color);
				if (!clipsXml.empty() && !fillEvents(tb, clipsXml, err)) {
					arrOk = false;
					break;
				}
				clones += sep + tb.xml;
			}
		}
		if (!arrOk) break;

		// Reassemble, then patch the globals on the whole document.
		xml = head;
		for (int t = 0; t < 8; t++) {
			xml += tracks[t].xml;
			if (t < 7) xml += sep;
		}
		xml += clones;
		xml += tail;

		if (!setValue(xml, 0, "NextPointeeId", std::to_string(nextPointee), err)) break;
		// Set tempo: the <Tempo><Manual> inside the MasterTrack's mixer (the only Tempo
		// holder in a Live 11 set — the PreHearTrack has none) — AND the master's tempo
		// AUTOMATION envelope: the template carries an envelope targeting the Tempo's
		// AutomationTarget pointee with the saved tempo as its initial FloatEvent, and
		// Live shows THAT value, not Manual (found 2026-08-26: sets opened at the
		// template's 109.81 with Manual correctly patched).
		{
			size_t h = xml.find("<MasterTrack>");
			size_t tempo = h == std::string::npos ? std::string::npos : xml.find("<Tempo>", h);
			if (tempo == std::string::npos) {
				err.fail("template: no <Tempo> in <MasterTrack>");
				break;
			}
			if (!setValue(xml, tempo, "Manual", num(p.tempo), err)) break;
			size_t masterEnd = xml.find("</MasterTrack>", h);
			const std::string tgtPat = "<AutomationTarget Id=\"";
			size_t tgt = xml.find(tgtPat, tempo);
			if (tgt != std::string::npos && tgt < masterEnd) {
				long pointee = std::strtol(xml.c_str() + tgt + tgtPat.size(), nullptr, 10);
				const std::string pePat = "<PointeeId Value=\"" + std::to_string(pointee) + "\"";
				size_t pe = xml.find(pePat, h);
				if (pe != std::string::npos && pe < masterEnd) {
					size_t envEnd = xml.find("</AutomationEnvelope>", pe);
					size_t fp = pe;
					const std::string fePat = "<FloatEvent ";
					const std::string valPat = "Value=\"";
					while ((fp = xml.find(fePat, fp)) != std::string::npos && fp < envEnd) {
						size_t vs = xml.find(valPat, fp);
						if (vs == std::string::npos || vs > envEnd) break;
						vs += valPat.size();
						size_t ve = xml.find('"', vs);
						xml.replace(vs, ve - vs, num(p.tempo));
						envEnd = xml.find("</AutomationEnvelope>", vs); // xml shifted
						fp = vs;
					}
				}
			}
		}

		// Transport: the donor set ships with its author's loop brace and parked
		// playhead — replace with ours: playhead at bar 1, loop brace spanning the
		// whole jam (rounded up to bars), loop off. Best-effort (a missing field is
		// no reason to fail the export).
		{
			size_t tr = xml.find("<Transport>");
			if (tr != std::string::npos) {
				double bars = maxEndBeats > 0 ? (double) ((long) ((maxEndBeats + 3.999) / 4)) * 4 : 16;
				Err ignore;
				setValue(xml, tr, "LoopOn", "false", ignore);
				setValue(xml, tr, "LoopStart", "0", ignore);
				setValue(xml, tr, "LoopLength", num(bars), ignore);
				setValue(xml, tr, "CurrentTime", "0", ignore);
			}
		}

		// Hardening: a generated set must be well-formed before it ever reaches Live —
		// one slipped splice otherwise surfaces only as Live refusing the file. Walk
		// every tag and check open/close balance (self-closing and <?...?> skipped).
		{
			int depth = 0;
			bool ok = true;
			for (size_t i = 0; ok && (i = xml.find('<', i)) != std::string::npos; i++) {
				size_t end = xml.find('>', i);
				if (end == std::string::npos) { ok = false; break; }
				if (xml[i + 1] == '?') { i = end; continue; }
				if (xml[i + 1] == '/') depth--;
				else if (xml[end - 1] != '/') depth++;
				if (depth < 0) ok = false;
				i = end;
			}
			if (!ok || depth != 0) {
				err.fail("internal: generated XML is unbalanced — refusing to write");
				break;
			}
		}
		if (errOut) errOut->clear();
		return xml;
	} while (false);

	if (errOut) *errOut = err.msg;
	return std::string();
}

// ---- Looper "as played" lanes ----------------------------------------------------

void buildLooperLanes(const std::vector<LoopEventIn>& events,
                      const std::vector<LooperTakeIn>& takes,
                      const std::string trackNames[8], uint64_t timelineEnd,
                      double tempo, AlsProject& out) {
	for (int t = 0; t < out.looperTracks && t < 8; t++) {
		// This track's events, in timeline order (the log is appended chronologically;
		// a stable insertion sort guards against any out-of-order tail).
		std::vector<const LoopEventIn*> evs;
		for (const LoopEventIn& e : events)
			if (e.track == t) evs.push_back(&e);
		std::stable_sort(evs.begin(), evs.end(),
			[](const LoopEventIn* a, const LoopEventIn* b) {
				return a->sessionFrame < b->sessionFrame;
			});

		std::string laneName = !trackNames[t].empty() ? trackNames[t]
		                       : ("Track " + std::to_string(t + 1));
		std::vector<AlsArrangementClip>& lane = out.looperLanes[t];

		bool open = false;
		LoopEventIn cur;                 // the START that opened the current span
		auto close = [&](uint64_t endSf) {
			if (!open) return;
			open = false;
			// Rest spans (silence steps) and zero-length spans produce no clip.
			if (cur.rest || endSf <= cur.sessionFrame) return;
			// Take-identity rule: the audio must still be the take that played.
			const LooperTakeIn* take = nullptr;
			for (const LooperTakeIn& k : takes)
				if (k.track == cur.track && k.slot == cur.slot
				        && k.startFrame == cur.takeStartFrame) { take = &k; break; }
			if (!take) return;           // re-recorded since: audio is in history/, skip
			AlsArrangementClip c;
			c.name = laneName + " s" + std::to_string(cur.slot + 1);
			c.relPath = "looper/" + take->file;
			c.absPath = take->absPath;
			c.sessionFrame = cur.sessionFrame;
			c.endSessionFrame = endSf;
			c.loop = true;
			double sr = take->sampleRate > 0 ? take->sampleRate : 48000.0;
			double bpm = cur.bpm > 0 ? (double) cur.bpm : tempo;
			c.loopLenBeats = takeLenBeats(take->frames, sr, bpm, cur.bpi);
			c.frames = take->frames;
			c.sampleRate = sr;
			c.fileSize = take->fileSize;
			c.mtime = take->mtime;
			lane.push_back(c);
		};
		for (const LoopEventIn* e : evs) {
			if (e->start) {
				close(e->sessionFrame);  // monophonic: a START closes any open span
				open = true;
				cur = *e;
			} else if (open && e->slot == cur.slot) {
				close(e->sessionFrame);
			}
			// A STOP for a slot that isn't the open span (a lost/duplicate event) is
			// ignored — the open span keeps running until its own STOP or a new START.
		}
		close(timelineEnd);              // unpaired START: the span ran to the end
	}
}

} // namespace looper
} // namespace akaudio
