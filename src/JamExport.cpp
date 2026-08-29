// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "JamExport.hpp"

#include "plugin.hpp"
#include "looper/AlsExport.hpp"
#include "dep/stb_vorbis_impl.hpp"
#include "net/ninjam/NjEncoder.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <sys/stat.h>

namespace akaudio {

namespace {

constexpr int TRACKS = 8;

std::string readFile(const std::string& p) {
	std::ifstream f(p, std::ios::binary);
	if (!f) return "";
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
long fileSizeOf(const std::string& p) {
	return (long) system::getFileSize(p); // fs-based: UTF-8 + both separators on Windows
}
long fileMtimeOf(const std::string& p) {
	struct stat st;
	return stat(p.c_str(), &st) == 0 ? (long) st.st_mtime : 0;
}
// True if the file begins a standalone Ogg-Vorbis stream: a BOS page carrying the
// Vorbis ID header. Wire archives written before the txArchWhole fix (NjClient) can
// hold a first tx row that is a headerless mid-interval slice — undecodable by Live
// (or anything), so the export skips such rows.
bool standaloneOggFile(const std::string& p) {
	std::ifstream f(p, std::ios::binary);
	char h[35];
	if (!f.read(h, sizeof(h))) return false;
	return std::memcmp(h, "OggS", 4) == 0 && ((unsigned char) h[5] & 0x02)
	       && std::memcmp(h + 28, "\x01vorbis", 7) == 0;
}

// Decode a whole OGG to interleaved stereo float (mono fanned out). Empty on failure.
std::vector<float> decodeOggStereo(const std::string& path, int& rate) {
	std::vector<float> pcm;
	std::ifstream f(path, std::ios::binary);
	if (!f) return pcm;
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (bytes.empty()) return pcm;
	int err = 0;
	stb_vorbis* v = stb_vorbis_open_memory(bytes.data(), (int) bytes.size(), &err, nullptr);
	if (!v) return pcm;
	stb_vorbis_info info = stb_vorbis_get_info(v);
	int nch = info.channels;
	rate = (int) info.sample_rate;
	if (nch < 1 || nch > 2) { stb_vorbis_close(v); return pcm; }
	std::vector<float> raw;
	const int CHUNK = 4096;
	std::vector<float> tmp((size_t) CHUNK * nch);
	for (;;) {
		int got = stb_vorbis_get_samples_float_interleaved(v, nch, tmp.data(), CHUNK * nch);
		if (got <= 0) break;
		raw.insert(raw.end(), tmp.begin(), tmp.begin() + (size_t) got * nch);
	}
	stb_vorbis_close(v);
	long n = (long) (raw.size() / (size_t) nch);
	pcm.resize((size_t) n * 2);
	for (long i = 0; i < n; i++) {
		float l = raw[(size_t) i * nch];
		float r = nch >= 2 ? raw[(size_t) i * nch + 1] : l;
		pcm[(size_t) i * 2] = l;
		pcm[(size_t) i * 2 + 1] = r;
	}
	return pcm;
}

// One archived player interval, collected for the Lite merge.
struct PlayerRow {
	std::string abs, rel;
	uint64_t sf = 0;
	long frames = 0, fileSize = 0, mtime = 0, seq = 0;
	double sampleRate = 48000;
};

} // namespace

std::string exportJamAls(const std::string& jamRoot, bool liteMode, std::string* whyNot) {
	akaudio::looper::AlsProject proj;
	proj.title = system::getFilename(jamRoot);
	// Live Lite flavor: 6 grid tracks; players merged onto one lane; players + TX use
	// the freed template tracks 7+8 (no clones — Lite caps at 8 audio tracks).
	const int gridTracks = liteMode ? 6 : TRACKS;
	proj.looperTracks = gridTracks;
	proj.inlineArrangement = liteMode;
	int droppedGrid = 0;
	double tempo = 0.0;
	uint64_t timelineEnd = 0;
	long ivFrames = 0; // interval length in frames (first seen) — timeline-rebase grain

	// Looper session → Session-View clips (+ the take list the lane builder matches
	// play-spans against, via the startFrame capture identity).
	std::vector<akaudio::looper::LooperTakeIn> takesIn;
	std::string looperDir = jamRoot + "/looper";
	std::string sjson = readFile(looperDir + "/session.json");
	if (!sjson.empty()) {
		json_error_t err;
		if (json_t* root = json_loads(sjson.c_str(), 0, &err)) {
			if (json_t* b = json_object_get(root, "bpm"))
				if (json_number_value(b) > 0) tempo = json_number_value(b);
			json_t* tracks = json_object_get(root, "tracks");
			if (json_is_array(tracks)) {
				size_t i; json_t* v;
				json_array_foreach(tracks, i, v) {
					int idx = (int) json_integer_value(json_object_get(v, "index"));
					const char* nm = json_string_value(json_object_get(v, "name"));
					if (idx >= 0 && idx < TRACKS && nm) proj.looperTrackNames[idx] = nm;
				}
			}
			json_t* slots = json_object_get(root, "slots");
			if (json_is_array(slots)) {
				size_t i; json_t* v;
				json_array_foreach(slots, i, v) {
					const char* file = json_string_value(json_object_get(v, "file"));
					if (!file || !*file) continue;  // settings-only rest cell
					akaudio::looper::AlsSessionClip c;
					c.track = (int) json_integer_value(json_object_get(v, "track"));
					c.scene = (int) json_integer_value(json_object_get(v, "slot"));
					c.relPath = std::string("looper/") + file;
					c.absPath = looperDir + "/" + file;
					c.frames = (long) json_integer_value(json_object_get(v, "frames"));
					c.sampleRate = json_number_value(json_object_get(v, "sampleRate"));
					c.bpm = json_number_value(json_object_get(v, "bpm"));
					c.bpi = (int) json_integer_value(json_object_get(v, "bpi"));
					if (c.bpm <= 0) c.bpm = tempo > 0 ? tempo : 120;
					if (c.sampleRate <= 0) c.sampleRate = 48000;
					c.fileSize = fileSizeOf(c.absPath);
					c.mtime = fileMtimeOf(c.absPath);
					if (c.fileSize > 0 && !standaloneOggFile(c.absPath)) {
						WARN("akaudio: .als export: skipping non-standalone OGG %s", c.absPath.c_str());
						continue;
					}
					if (c.track < 0 || c.track >= TRACKS) continue;
					if (c.track >= gridTracks) { droppedGrid++; continue; }
					c.name = !proj.looperTrackNames[c.track].empty()
					         ? proj.looperTrackNames[c.track] : ("clip " + std::to_string(c.scene + 1));
					proj.sessionClips.push_back(c);

					akaudio::looper::LooperTakeIn k;
					k.track = c.track;
					k.slot = c.scene;
					k.file = file;
					k.absPath = c.absPath;
					k.frames = c.frames;
					k.sampleRate = c.sampleRate;
					k.bpm = (int) c.bpm;
					k.bpi = c.bpi;
					k.startFrame = (uint64_t) json_integer_value(json_object_get(v, "startFrame"));
					k.fileSize = c.fileSize;
					k.mtime = c.mtime;
					takesIn.push_back(k);
				}
			}
			json_decref(root);
		}
	}

	// Recorder wire archive → Arrangement tracks (one per player + TX).
	std::vector<PlayerRow> playerRows; // Lite: all players, merged below
	std::string idxAll = readFile(jamRoot + "/index.jsonl");
	// The boundary snap below assumes ONE grid for the whole session, anchored at
	// session frame 0. After a mid-session tempo change the later boundaries sit at
	// changePoint + k·newFrames (JamClock keeps the timeline continuous), NOT on the
	// zero-anchored lattice — snapping there would actively misplace clips by up to
	// half an interval. Detect mixed interval lengths up front and fall back to raw
	// stamps for the whole file (the pre-snap graceful degradation), with a warning.
	bool uniformGrid = true;
	if (!idxAll.empty()) {
		long seenFrames = 0;
		std::istringstream ss0(idxAll);
		std::string line0;
		while (std::getline(ss0, line0)) {
			if (line0.empty()) continue;
			json_error_t e0;
			json_t* o0 = json_loads(line0.c_str(), 0, &e0);
			if (!o0) continue;
			long fr = (long) json_integer_value(json_object_get(o0, "frames"));
			json_decref(o0);
			if (fr <= 0) continue;
			if (seenFrames == 0) seenFrames = fr;
			else if (fr != seenFrames) { uniformGrid = false; break; }
		}
		if (!uniformGrid)
			WARN("akaudio: .als export: tempo change detected in index.jsonl — "
			     "clips placed at raw stamps (no grid snap)");
	}
	if (!idxAll.empty()) {
		std::map<std::string, size_t> byKey;
		std::istringstream ss(idxAll);
		std::string line;
		while (std::getline(ss, line)) {
			if (line.empty()) continue;
			json_error_t err;
			json_t* o = json_loads(line.c_str(), 0, &err);
			if (!o) continue;
			bool tx = json_is_true(json_object_get(o, "tx"));
			const char* user = json_string_value(json_object_get(o, "user"));
			int chidx = (int) json_integer_value(json_object_get(o, "chidx"));
			const char* file = json_string_value(json_object_get(o, "file"));
			long frames = (long) json_integer_value(json_object_get(o, "frames"));
			double sr = json_number_value(json_object_get(o, "sampleRate"));
			double bpm = json_number_value(json_object_get(o, "bpm"));
			long seq = (long) json_integer_value(json_object_get(o, "seq"));
			json_t* sf = json_object_get(o, "sessionFrame");
			std::string abs = file && *file ? jamRoot + "/" + file : "";
			long absSize = abs.empty() ? 0 : fileSizeOf(abs);
			if (absSize > 0 && !standaloneOggFile(abs)) {
				WARN("akaudio: .als export: skipping non-standalone OGG %s", abs.c_str());
				json_decref(o);
				continue;
			}
			// Snap the stamp to the nearest interval boundary. Interval audio is
			// downbeat-aligned by construction, so any residual offset in the stamp is
			// transport (clock-publish granularity, receive-chain phase), not music —
			// snapping puts every lane on the grid and makes consecutive clips tile.
			// Only on a uniform grid (see the pre-scan above): a session with a tempo
			// change keeps its raw stamps.
			uint64_t sfv = sf ? (uint64_t) json_integer_value(sf) : 0;
			if (uniformGrid && frames > 0)
				sfv = ((sfv + (uint64_t) frames / 2) / (uint64_t) frames) * (uint64_t) frames;
			if (file && *file && frames > 0) {
				if (tempo <= 0 && bpm > 0) tempo = bpm;
				if (!ivFrames) ivFrames = frames;
				if (liteMode && !tx) {
					// Lite: every player lands on ONE merged lane (built below).
					PlayerRow r;
					r.abs = abs;
					r.rel = file;
					r.sf = sfv;
					r.frames = frames;
					r.sampleRate = sr > 0 ? sr : 48000;
					r.fileSize = absSize;
					r.mtime = fileMtimeOf(abs);
					r.seq = seq;
					playerRows.push_back(r);
					if (r.sf + (uint64_t) frames > timelineEnd)
						timelineEnd = r.sf + (uint64_t) frames;
					json_decref(o);
					continue;
				}
				std::string u = user ? user : "player";
				std::string key = tx ? "tx" : (u + "#" + std::to_string(chidx));
				size_t ti;
				auto it = byKey.find(key);
				if (it == byKey.end()) {
					akaudio::looper::AlsArrangementTrack tr;
					tr.isTx = tx;
					tr.name = tx ? "you (tx)" : (u + (chidx > 0 ? " ch" + std::to_string(chidx) : ""));
					ti = proj.arrangementTracks.size();
					proj.arrangementTracks.push_back(tr);
					byKey[key] = ti;
				} else {
					ti = it->second;
				}
				akaudio::looper::AlsArrangementClip c;
				c.name = (tx ? "tx " : (u + " ")) + std::to_string(seq);
				c.relPath = file;
				c.absPath = abs;
				c.sessionFrame = sfv;
				c.frames = frames;
				c.sampleRate = sr > 0 ? sr : 48000;
				c.fileSize = absSize;
				c.mtime = fileMtimeOf(c.absPath);
				if (c.sessionFrame + (uint64_t) frames > timelineEnd)
					timelineEnd = c.sessionFrame + (uint64_t) frames;
				proj.arrangementTracks[ti].clips.push_back(c);
			}
			json_decref(o);
		}
	}

	// Lite: merge every player onto one lane. Intervals are grouped by their quantized
	// interval index; a lone interval references its original OGG untouched, while
	// genuinely simultaneous intervals are decoded, summed, and encoded once into
	// <jamRoot>/mixdown/ (regenerated on each export).
	if (liteMode && !playerRows.empty()) {
		std::map<long, std::vector<const PlayerRow*>> groups;
		for (const auto& r : playerRows) {
			long idx = r.frames > 0 ? (long) std::llround((double) r.sf / (double) r.frames) : 0;
			groups[idx].push_back(&r);
		}
		akaudio::looper::AlsArrangementTrack pl;
		pl.name = "players";
		bool mixdirMade = false;
		int serial = 9000; // OGG stream serial space distinct from the session's takes
		for (const auto& g : groups) {
			const std::vector<const PlayerRow*>& rows = g.second;
			akaudio::looper::AlsArrangementClip c;
			if (rows.size() == 1) {
				const PlayerRow* r = rows[0];
				c.name = "players " + std::to_string(r->seq);
				c.relPath = r->rel;
				c.absPath = r->abs;
				c.sessionFrame = r->sf;
				c.frames = r->frames;
				c.sampleRate = r->sampleRate;
				c.fileSize = r->fileSize;
				c.mtime = r->mtime;
			} else {
				long maxFrames = 0;
				uint64_t minSf = rows[0]->sf;
				for (const PlayerRow* r : rows) {
					if (r->frames > maxFrames) maxFrames = r->frames;
					if (r->sf < minSf) minSf = r->sf;
				}
				int mixRate = 0;
				std::vector<float> mix((size_t) maxFrames * 2, 0.f);
				for (const PlayerRow* r : rows) {
					int rate = 0;
					std::vector<float> pcm = decodeOggStereo(r->abs, rate);
					if (pcm.empty()) continue;
					if (mixRate == 0) mixRate = rate;
					if (rate != mixRate) {
						WARN("akaudio: mixdown: rate mismatch, skipping %s", r->abs.c_str());
						continue;
					}
					long n = (long) (pcm.size() / 2);
					if (n > maxFrames) n = maxFrames;
					for (long i = 0; i < n * 2; i++)
						mix[(size_t) i] += pcm[(size_t) i];
				}
				for (float& v : mix)  // hard safety clamp; players arrive pre-leveled
					v = v > 1.f ? 1.f : (v < -1.f ? -1.f : v);
				if (mixRate == 0) continue; // nothing decoded
				std::vector<uint8_t> ogg = akaudio::nj::encodeOggInterval(
					mix.data(), (int) maxFrames, 2, mixRate, 0.8f, serial++);
				if (ogg.empty()) continue;
				if (!mixdirMade) {
					system::createDirectory(jamRoot + "/mixdown");
					mixdirMade = true;
				}
				char nm[48];
				(void) std::snprintf(nm, sizeof(nm), "mixdown/mix_%06ld.ogg", g.first);
				std::string abs = jamRoot + "/" + nm;
				{
					std::ofstream f(abs, std::ios::binary | std::ios::trunc);
					f.write((const char*) ogg.data(), (std::streamsize) ogg.size());
					if (!f) {
						WARN("akaudio: mixdown: could not write %s", abs.c_str());
						continue;
					}
				}
				c.name = "players mix " + std::to_string(g.first);
				c.relPath = nm;
				c.absPath = abs;
				c.sessionFrame = minSf;
				c.frames = maxFrames;
				c.sampleRate = mixRate;
				c.fileSize = (long) ogg.size();
				c.mtime = fileMtimeOf(abs);
			}
			pl.clips.push_back(c);
		}
		if (!pl.clips.empty())
			proj.arrangementTracks.insert(proj.arrangementTracks.begin(), pl);
	}
	if (droppedGrid > 0)
		WARN("akaudio: Lite flavor: %d take(s) on tracks 7-8 not exported (6-track grid)",
		     droppedGrid);

	// Performance log → per-track "as played" Arrangement lanes.
	std::vector<akaudio::looper::LoopEventIn> evsIn;
	std::string evAll = readFile(looperDir + "/events.jsonl");
	if (!evAll.empty()) {
		std::istringstream ss(evAll);
		std::string line;
		while (std::getline(ss, line)) {
			if (line.empty()) continue;
			json_error_t err;
			json_t* o = json_loads(line.c_str(), 0, &err);
			if (!o) continue;
			const char* kind = json_string_value(json_object_get(o, "ev"));
			akaudio::looper::LoopEventIn e;
			e.start = kind && std::strcmp(kind, "start") == 0;
			e.track = (int) json_integer_value(json_object_get(o, "t"));
			e.slot = (int) json_integer_value(json_object_get(o, "s"));
			e.sessionFrame = (uint64_t) json_integer_value(json_object_get(o, "sf"));
			e.takeStartFrame = (uint64_t) json_integer_value(json_object_get(o, "take"));
			e.rest = json_is_true(json_object_get(o, "rest"));
			e.bpm = (int) json_integer_value(json_object_get(o, "bpm"));
			e.bpi = (int) json_integer_value(json_object_get(o, "bpi"));
			e.sampleRate = json_number_value(json_object_get(o, "sr"));
			if (e.sampleRate <= 0) e.sampleRate = 48000;
			if (kind && e.track >= 0 && e.track < TRACKS) {
				if (e.sessionFrame > timelineEnd) timelineEnd = e.sessionFrame;
				if (!ivFrames && e.bpm > 0 && e.bpi > 0)
					ivFrames = std::lround(e.sampleRate * 60.0 / e.bpm * e.bpi);
				evsIn.push_back(e);
			}
			json_decref(o);
		}
	}

	// A final still-playing span would otherwise close at its own START frame (and be
	// dropped as zero-length): extend the timeline one interval past the last event.
	if (!evsIn.empty() && ivFrames > 0) {
		uint64_t lastEv = 0;
		for (const auto& e : evsIn)
			if (e.sessionFrame > lastEv) lastEv = e.sessionFrame;
		if (lastEv + (uint64_t) ivFrames > timelineEnd)
			timelineEnd = lastEv + (uint64_t) ivFrames;
	}

	if (proj.sessionClips.empty() && proj.arrangementTracks.empty() && evsIn.empty()) {
		if (whyNot) *whyNot = "No jam found in that folder.\nPick a jam folder that "
		                      "contains looper/session.json or index.jsonl.";
		return "";
	}
	proj.tempo = tempo > 0 ? tempo : 120;
	if (!evsIn.empty())
		akaudio::looper::buildLooperLanes(evsIn, takesIn, proj.looperTrackNames,
		                                  timelineEnd, proj.tempo, proj);

	// Rebase the timeline to the earliest archived moment, rounded down to a whole
	// interval so downbeats stay on bars. The session clock runs from the JOIN — a jam
	// armed minutes into a room would otherwise start 100+ empty bars in (found
	// 2026-08-25: first interval at frame 17.5M ⇒ bar ~122, "empty" arrangement).
	{
		uint64_t minSf = (uint64_t) -1;
		for (const auto& tr : proj.arrangementTracks)
			for (const auto& c : tr.clips)
				if (c.sessionFrame < minSf) minSf = c.sessionFrame;
		for (int t = 0; t < 8; t++)
			for (const auto& c : proj.looperLanes[t])
				if (c.sessionFrame < minSf) minSf = c.sessionFrame;
		if (minSf != (uint64_t) -1 && minSf > 0 && ivFrames > 0)
			proj.timelineOrigin = minSf - (minSf % (uint64_t) ivFrames);
	}

	std::string tpl = readFile(asset::plugin(pluginInstance, "res/als/Live11Template.xml"));
	std::string alsErr;
	std::vector<uint8_t> bytes = akaudio::looper::buildAls(proj, tpl, &alsErr);
	if (bytes.empty()) {
		WARN("akaudio: .als export failed: %s", alsErr.c_str());
		if (whyNot) *whyNot = "Export failed: " + alsErr;
		return "";
	}
	// The empty marker folder that makes Live treat the jam root as a Project —
	// without it Live 11 won't resolve the clips' project-relative sample paths
	// (RelativePathType 3) and reports every OGG as missing media (found 2026-08-25).
	system::createDirectory(jamRoot + "/Ableton Project Info");
	std::string out = jamRoot + "/" + system::getFilename(jamRoot) + ".als";
	std::ofstream f(out, std::ios::binary | std::ios::trunc);
	f.write((const char*) bytes.data(), (std::streamsize) bytes.size());
	if (!f) { // failbit persists from a failed open or a failed write — one check covers both
		if (whyNot) *whyNot = "Could not write " + out;
		return "";
	}
	return out;
}

} // namespace akaudio
