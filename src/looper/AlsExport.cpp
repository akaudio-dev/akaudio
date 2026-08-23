// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "AlsExport.hpp"

#include <cstdio>
#include <sstream>

namespace akaudio {
namespace looper {

// ---- gzip via stored (uncompressed) DEFLATE blocks ------------------------------
// A valid .gz stream Live inflates, with no zlib dependency: 10-byte gzip header, then
// DEFLATE stored blocks (BTYPE=00: [BFINAL bit][LEN][NLEN][raw]), CRC32 + ISIZE trailer.
// The .als XML is small, so skipping compression is fine.

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
		out.push_back(0x01);          // BFINAL=1, BTYPE=00
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

// ---- XML helpers -----------------------------------------------------------------

static std::string esc(const std::string& s) {
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
static std::string num(double v) {
	char b[64];
	std::snprintf(b, sizeof(b), "%.6f", v);
	// trim trailing zeros (Live tolerates them, but keep it tidy)
	std::string s = b;
	if (s.find('.') != std::string::npos) {
		size_t e = s.find_last_not_of('0');
		if (s[e] == '.') e--;
		s.erase(e + 1);
	}
	return s;
}

namespace {

// Small stateful XML builder: indentation + a monotonic Id source (Live wants unique Ids).
struct Xml {
	std::string s;
	int id = 0;
	int next() { return id++; }
	void raw(const std::string& t) { s += t; }
	void v(const char* name, const std::string& value) {
		s += "<"; s += name; s += " Value=\""; s += value; s += "\" />\n";
	}
	void vi(const char* name, long value) { v(name, std::to_string(value)); }
	void vb(const char* name, bool value) { v(name, value ? "true" : "false"); }
	void vf(const char* name, double value) { v(name, num(value)); }
	void open(const std::string& t) { s += "<"; s += t; s += ">\n"; }
	void close(const std::string& t) { s += "</"; s += t; s += ">\n"; }
};

double beatsFor(long frames, double sampleRate, double bpm) {
	if (sampleRate <= 0.0) return 0.0;
	return (double) frames / sampleRate * bpm / 60.0;
}

// A FileRef → the OGG on disk (absolute primary, relative fallback).
void fileRef(Xml& x, const std::string& absPath, const std::string& relPath, long fileSize) {
	x.open("FileRef");
	x.vi("RelativePathType", 3);              // project-relative
	x.v("RelativePath", esc(relPath));
	x.v("Path", esc(absPath));
	x.vi("Type", 1);
	x.v("LivePackName", "");
	x.v("LivePackId", "");
	x.v("OriginalFileSize", std::to_string(fileSize));
	x.vi("OriginalCrc", 0);
	x.close("FileRef");
}

// One AudioClip (used in both Session slots and the Arrangement). `startBeats` positions
// it (0 for a session clip); `loop` makes it a looping clip (the Looper takes).
void audioClip(Xml& x, const std::string& name, double startBeats, double lenBeats,
               double clipSeconds, bool loop, const std::string& absPath,
               const std::string& relPath, long frames, double sampleRate, long fileSize,
               int color) {
	double endBeats = startBeats + lenBeats;
	x.s += "<AudioClip Id=\"" + std::to_string(x.next()) + "\" Time=\"" + num(startBeats) + "\">\n";
	x.vi("LomId", 0);
	x.vi("LomIdView", 0);
	x.vf("CurrentStart", startBeats);
	x.vf("CurrentEnd", endBeats);
	x.open("Loop");
	x.vf("LoopStart", 0);
	x.vf("LoopEnd", lenBeats);
	x.vf("StartRelative", 0);
	x.vb("LoopOn", loop);
	x.vf("OutMarker", lenBeats);
	x.vf("HiddenLoopStart", 0);
	x.vf("HiddenLoopEnd", lenBeats);
	x.close("Loop");
	x.v("Name", esc(name));
	x.v("Annotation", "");
	x.vi("ColorIndex", color);
	x.vi("LaunchMode", 0);
	x.vi("LaunchQuantisation", 0);
	x.open("TimeSignature");
	x.open("TimeSignatures");
	x.raw("<RemoteableTimeSignature Id=\"0\">\n");
	x.vi("Numerator", 4);
	x.vi("Denominator", 4);
	x.vi("Time", 0);
	x.close("RemoteableTimeSignature");
	x.close("TimeSignatures");
	x.close("TimeSignature");
	x.open("Envelopes");
	x.raw("<Envelopes />\n");
	x.close("Envelopes");
	x.open("ScrollerTimePreserver");
	x.vf("LeftTime", 0);
	x.vf("RightTime", clipSeconds);
	x.close("ScrollerTimePreserver");
	x.open("TimeSelection");
	x.vi("AnchorTime", 0);
	x.vi("OtherTime", 0);
	x.close("TimeSelection");
	x.vb("Legato", false);
	x.vb("Ram", false);
	x.open("GrooveSettings");
	x.vi("GrooveId", -1);
	x.close("GrooveSettings");
	x.vb("Disabled", false);
	x.vi("VelocityAmount", 0);
	x.open("FollowAction");
	x.vf("FollowTime", 4);
	x.vb("IsLinked", true);
	x.vi("LoopIterations", 1);
	x.vi("FollowActionA", 0);
	x.vi("FollowActionB", 0);
	x.vi("FollowChanceA", 100);
	x.vi("FollowChanceB", 0);
	x.close("FollowAction");
	x.open("Grid");
	x.vi("FixedNumerator", 1);
	x.vi("FixedDenominator", 16);
	x.vi("GridIntervalPixel", 20);
	x.vi("Ntoles", 2);
	x.vb("SnapToGrid", true);
	x.vb("Fixed", false);
	x.close("Grid");
	x.vi("FreezeStart", 0);
	x.vi("FreezeEnd", 0);
	x.vb("IsWarped", false);
	x.vi("TakeId", 1);
	x.open("SampleRef");
	fileRef(x, absPath, relPath, fileSize);
	x.vi("LastModDate", 0);
	x.raw("<SourceContext />\n");
	x.vi("SampleUsageHint", 0);
	x.v("DefaultDuration", std::to_string(frames));
	x.vf("DefaultSampleRate", sampleRate);
	x.close("SampleRef");
	x.open("Onsets");
	x.raw("<UserOnsets />\n");
	x.vb("HasUserOnsets", false);
	x.close("Onsets");
	x.v("WarpMode", "0");
	x.open("WarpMarkers");
	x.raw("<WarpMarker Id=\"0\" SecTime=\"0\" BeatTime=\"0\" />\n");
	x.raw("<WarpMarker Id=\"1\" SecTime=\"" + num(clipSeconds) + "\" BeatTime=\"" + num(lenBeats) + "\" />\n");
	x.close("WarpMarkers");
	x.raw("<SavedWarpMarkersForStretched />\n");
	x.vb("MarkersGenerated", false);
	x.vb("IsSongTempoLeader", false);
	x.close("AudioClip");
}

// <Tag Id="N"><LockEnvelope Value="0" /></Tag> — the automation hook every mixer/tempo
// param carries.
void automationTarget(Xml& x, const char* tag) {
	x.s += "<"; x.s += tag; x.s += " Id=\"" + std::to_string(x.next()) + "\">\n";
	x.vi("LockEnvelope", 0);
	x.s += "</"; x.s += tag; x.s += ">\n";
}

// A continuous mixer param: <Tag><LomId/><Manual/><MidiControllerRange/><AutomationTarget/>
// <ModulationTarget/></Tag>.
void floatParam(Xml& x, const char* tag, double manual, double lo, double hi) {
	x.open(tag);
	x.vi("LomId", 0);
	x.vf("Manual", manual);
	x.open("MidiControllerRange");
	x.vf("Min", lo);
	x.vf("Max", hi);
	x.close("MidiControllerRange");
	automationTarget(x, "AutomationTarget");
	x.s += "<ModulationTarget Id=\"" + std::to_string(x.next()) + "\">\n";
	x.vi("LockEnvelope", 0);
	x.close("ModulationTarget");
	x.close(tag);
}

// A minimal but complete AudioTrack mixer: On, Volume, Pan, no sends (no return tracks).
void mixer(Xml& x) {
	x.open("Mixer");
	x.vi("LomId", 0);
	x.vb("LomIdView", false);
	x.vi("IsExpanded", true);
	// On (bool param).
	x.open("On");
	x.vi("LomId", 0);
	x.vb("Manual", true);
	automationTarget(x, "AutomationTarget");
	x.close("On");
	x.raw("<Sends />\n");
	x.open("Speaker");
	x.vi("LomId", 0);
	x.vb("Manual", true);
	automationTarget(x, "AutomationTarget");
	x.close("Speaker");
	x.vi("SoloSink", 0);
	x.vb("PanMoveFactor", false);
	floatParam(x, "Volume", 1.0, 0.0, 1.0);
	floatParam(x, "Pan", 0.0, -1.0, 1.0);
	x.close("Mixer");
}

} // namespace

// ---- The set ---------------------------------------------------------------------

// One AudioTrack: `sessionClips` fill its scene slots (by .scene); `arr` clips go on the
// Arrangement. Either list may be empty (Looper tracks vs player tracks).
static void audioTrack(Xml& x, const std::string& name, int color, int nScenes,
                       const std::vector<const AlsSessionClip*>& sessionClips,
                       const std::vector<AlsArrangementClip>* arr, double tempo) {
	x.s += "<AudioTrack Id=\"" + std::to_string(x.next()) + "\">\n";
	x.vi("LomId", 0);
	x.vi("LomIdView", 0);
	x.vb("IsContentSelectedInDocument", false);
	x.vi("TrackGroupId", -1);
	x.vb("TrackUnfolded", true);
	x.open("Name");
	x.v("EffectiveName", esc(name));
	x.v("UserName", esc(name));
	x.v("Annotation", "");
	x.v("MemorizedFirstClipName", "");
	x.close("Name");
	x.vi("Color", color);
	x.open("DeviceChain");
	// Routing: default audio in/out to master.
	x.open("AudioInputRouting");
	x.v("Target", "AudioIn/External/S0");
	x.v("UpperDisplayString", "Ext. In");
	x.v("LowerDisplayString", "1/2");
	x.close("AudioInputRouting");
	x.open("AudioOutputRouting");
	x.v("Target", "AudioOut/Master");
	x.v("UpperDisplayString", "Master");
	x.v("LowerDisplayString", "");
	x.close("AudioOutputRouting");
	mixer(x);
	x.open("MainSequencer");
	x.vi("LomId", 0);
	x.open("ClipSlotList");
	for (int sc = 0; sc < nScenes; sc++) {
		const AlsSessionClip* clip = nullptr;
		for (const AlsSessionClip* c : sessionClips)
			if (c->scene == sc) { clip = c; break; }
		x.s += "<ClipSlot Id=\"" + std::to_string(x.next()) + "\">\n";
		x.vi("LomId", 0);
		x.open("ClipSlot");
		if (clip) {
			x.open("Value");
			double len = beatsFor(clip->frames, clip->sampleRate, clip->bpm);
			double secs = clip->sampleRate > 0 ? (double) clip->frames / clip->sampleRate : 0.0;
			audioClip(x, clip->name, 0, len, secs, /*loop=*/true, clip->absPath, clip->relPath,
			          clip->frames, clip->sampleRate, clip->fileSize, color);
			x.close("Value");
		} else {
			x.raw("<Value />\n");
		}
		x.close("ClipSlot");
		x.vb("HasStop", true);
		x.vb("NeedRefreeze", true);
		x.close("ClipSlot");
	}
	x.close("ClipSlotList");
	x.vi("MonitoringEnum", 1);
	x.open("Sample");
	x.open("ArrangerAutomation");
	x.open("Events");
	if (arr) {
		for (const AlsArrangementClip& c : *arr) {
			double start = beatsFor((long) c.sessionFrame, c.sampleRate, tempo);
			double len = beatsFor(c.frames, c.sampleRate, tempo);
			double secs = c.sampleRate > 0 ? (double) c.frames / c.sampleRate : 0.0;
			audioClip(x, c.name, start, len, secs, /*loop=*/false, c.absPath, c.relPath,
			          c.frames, c.sampleRate, c.fileSize, color);
		}
	}
	x.close("Events");
	x.close("ArrangerAutomation");
	x.close("Sample");
	x.close("MainSequencer");
	x.close("DeviceChain");
	x.close("AudioTrack");
}

std::string buildAlsXml(const AlsProject& p) {
	Xml x;
	x.id = 1; // reserve 0 for the various LomId="0"
	const int nScenes = 8;

	x.raw("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
	x.raw("<Ableton MajorVersion=\"5\" MinorVersion=\"11.0_11300\" SchemaChangeCount=\"3\" "
	      "Creator=\"akaudio " "\" Revision=\"\">\n");
	x.open("LiveSet");
	x.vi("NextPointeeId", 100000);
	x.vi("OverwriteProtectionNumber", 2560);
	x.vi("LomId", 0);
	x.open("Tracks");

	int color = 0;
	// Looper tracks (Session-View clips).
	for (int t = 0; t < 8; t++) {
		std::vector<const AlsSessionClip*> clips;
		for (const AlsSessionClip& c : p.sessionClips)
			if (c.track == t) clips.push_back(&c);
		std::string name = p.looperTrackNames[t].empty() ? ("Loop " + std::to_string(t + 1))
		                                                  : p.looperTrackNames[t];
		audioTrack(x, name, color % 70, nScenes, clips, nullptr, p.tempo);
		color += 3;
	}
	// Arrangement tracks (players + our TX).
	std::vector<const AlsSessionClip*> none;
	for (const AlsArrangementTrack& tr : p.arrangementTracks) {
		audioTrack(x, tr.name, color % 70, nScenes, none, &tr.clips, p.tempo);
		color += 5;
	}
	x.close("Tracks");

	// Master track — carries the set tempo + time signature.
	x.open("MainTrack");
	x.open("DeviceChain");
	x.open("Mixer");
	x.open("Tempo");
	x.vi("LomId", 0);
	x.vf("Manual", p.tempo);
	x.open("MidiControllerRange");
	x.vf("Min", 60);
	x.vf("Max", 200);
	x.close("MidiControllerRange");
	automationTarget(x, "AutomationTarget");
	x.close("Tempo");
	x.open("TimeSignature");
	x.vi("LomId", 0);
	x.vi("Manual", p.meterNum);
	automationTarget(x, "AutomationTarget");
	x.close("TimeSignature");
	x.close("Mixer");
	x.close("DeviceChain");
	x.open("Name");
	x.v("EffectiveName", "Master");
	x.v("UserName", "");
	x.close("Name");
	x.close("MainTrack");

	// Scenes (8, to match every track's ClipSlotList length).
	x.open("Scenes");
	for (int sc = 0; sc < nScenes; sc++) {
		x.s += "<Scene Id=\"" + std::to_string(x.next()) + "\">\n";
		x.v("Name", "");
		x.vi("ColorIndex", -1);
		x.vf("Tempo", p.tempo);
		x.vb("IsTempoEnabled", false);
		x.vi("TimeSignatureId", 201);
		x.vb("IsTimeSignatureEnabled", false);
		x.close("Scene");
	}
	x.close("Scenes");

	x.open("Transport");
	x.vf("PhaseNudgeTempo", p.tempo);
	x.vb("LoopOn", false);
	x.vf("LoopStart", 0);
	x.vf("LoopLength", 16);
	x.vi("CurrentTime", 0);
	x.close("Transport");

	x.close("LiveSet");
	x.close("Ableton");
	return x.s;
}

} // namespace looper
} // namespace akaudio
