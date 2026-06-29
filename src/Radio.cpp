// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "plugin.hpp"
#include "net/Stream.hpp"
#include "net/StationImport.hpp"
#include "ClickableLed.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>

// Fundamental-style theme (matches the Ninjam panel): silver panel, dark labels.
static const char* RADIO_FONT_BOLD = "res/fonts/Nunito-Bold.ttf";
static const NVGcolor RADIO_TEXT     = nvgRGB(0x1f, 0x1f, 0x1f); // panel text, exactly like core/Fundamental
static const NVGcolor RADIO_TEXT_DIM = nvgRGB(0x6a, 0x72, 0x7a); // secondary labels
static const NVGcolor RADIO_TEXT_LT  = nvgRGB(0xf0, 0xf0, 0xf0); // labels on the dark output plate (Audio uses #f0f0f0)
static const NVGcolor RADIO_PLATE    = nvgRGB(0x1f, 0x1f, 0x1f); // Fundamental output-plate black

static void radioText(NVGcontext* vg, const char* fontRes, float x, float y, float size,
		NVGcolor col, const char* s) {
	std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system(fontRes));
	if (!font || font->handle < 0)
		return;
	nvgFontFaceId(vg, font->handle);
	nvgFontSize(vg, size);
	nvgFillColor(vg, col);
	nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
	nvgText(vg, x, y, s, NULL);
}

// Code-drawn panel decoration (NanoSVG ignores <text>), matching VCV's core/
// Fundamental house style: black Nunito-Bold title, LEVEL knob endpoint labels,
// and a dark #1F1F1F rounded output plate with light LEFT/RIGHT labels.
static const NVGcolor RADIO_TITLE = nvgRGB(0x1f, 0x1f, 0x1f); // core/Fundamental title color (#1f1f1f, not pure black)

// Output plate geometry (Fundamental: 39.16 px tall, r 2.83 px on a 75 px panel →
// 13.26 mm / 0.96 mm), centred on this 8 HP panel.
static const float PLATE_X = 3.9f, PLATE_Y = 104.66f, PLATE_W = 32.84f, PLATE_H = 13.26f;
static const float OUT_L_X = 13.32f, OUT_R_X = 27.32f, OUT_Y = 113.115f; // Audio output row

struct RadioDecor : Widget {
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		// Dark output plate (outputs sit on it; drawn low-z so jacks render on top).
		nvgBeginPath(vg);
		nvgRoundedRect(vg, mm2px(PLATE_X), mm2px(PLATE_Y), mm2px(PLATE_W), mm2px(PLATE_H), mm2px(0.96f));
		nvgFillColor(vg, RADIO_PLATE);
		nvgFill(vg);

		radioText(vg, RADIO_FONT_BOLD, mm2px(20.32f), mm2px(9.0f), 15.f, RADIO_TITLE, "RADIO");
		radioText(vg, RADIO_FONT_BOLD, mm2px(20.32f), mm2px(66.5f), 11.f, RADIO_TEXT, "LEVEL");

		// Gauge ring around the LEVEL knob — the open arc (gap at the bottom) the
		// AUDIO/Fundamental knobs use: radius 7.09 mm, stroke #1f1f1f 0.8 px.
		const float kx = mm2px(20.32f), ky = mm2px(77.362f), kr = mm2px(7.09f);
		nvgBeginPath(vg);
		nvgArc(vg, kx, ky, kr, nvgDegToRad(121.f), nvgDegToRad(419.f), NVG_CW);
		nvgStrokeColor(vg, RADIO_TEXT);
		nvgStrokeWidth(vg, 0.8f);
		nvgLineCap(vg, NVG_ROUND);
		nvgStroke(vg);

		// LEVEL endpoints at the arc's gap, like AUDIO.
		radioText(vg, RADIO_FONT_BOLD, mm2px(13.0f), mm2px(86.5f), 9.f, RADIO_TEXT, "-\xe2\x88\x9e"); // -∞
		radioText(vg, RADIO_FONT_BOLD, mm2px(27.6f), mm2px(86.5f), 9.f, RADIO_TEXT, "+12");
		radioText(vg, RADIO_FONT_BOLD, mm2px(20.32f), mm2px(91.5f), 10.f, RADIO_TEXT, "CV");
		// LEFT / RIGHT labels on the output plate.
		radioText(vg, RADIO_FONT_BOLD, mm2px(OUT_L_X), mm2px(PLATE_Y + 3.1f), 8.5f, RADIO_TEXT_LT, "LEFT");
		radioText(vg, RADIO_FONT_BOLD, mm2px(OUT_R_X), mm2px(PLATE_Y + 3.1f), 8.5f, RADIO_TEXT_LT, "RIGHT");
		// "AK" maker mark at the bottom, in the spot VCV uses for its logo — large + bold.
		radioText(vg, RADIO_FONT_BOLD, mm2px(20.32f), mm2px(121.0f), 16.f, RADIO_TEXT, "AK");
		Widget::draw(args);
	}
};

// Streaming internet radio (Icecast/HTTP MP3) source. Audio is fetched and decoded
// on a background thread (net/Stream.hpp) and pulled here on the audio thread.
struct Radio : Module {
	enum ParamId {
		LEVEL_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		LEVEL_INPUT, // CV modulation for the built-in VCA (unipolar 0–10V)
		INPUTS_LEN
	};
	enum OutputId {
		LEFT_OUTPUT,
		RIGHT_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		PLAYING_LIGHT,
		LIGHTS_LEN
	};

	akaudio::StreamClient stream;
	// "Add a station from a URL" worker: verify → identify (radio-browser) →
	// favicon → save a user preset, all off-thread. The widget polls it.
	akaudio::StationImporter importer;
	// Default to a calm ambient bed; "stations" are factory presets
	// (sound sources for soundscapes — nature, scanners, space — not music).
	std::string url = "https://nature-rex.radioca.st/stream";
	// Human label for the current station (shown on the panel, persisted in
	// presets). See presets/Radio/ for the grouped set.
	std::string stationName = "Ambi Nature Radio";
	// Plugin-relative path to the station's bundled artwork (e.g.
	// "stations/bbcworldservice.png"), shown on the panel. Empty = ♪ placeholder.
	std::string icon = "";
	bool playing = false;

	// --- Audition / import coordination (UI thread only) ---
	// Snapshot of the station before an audition, restored if the audition fails.
	std::string prevUrl, prevName, prevIcon;
	bool prevPlaying = false;
	// Transient status shown on the panel (e.g. "Auditioning…", "✕ No audio",
	// "Added …"), with a frame TTL so outcomes clear themselves.
	std::string importMsg;
	bool importError = false;
	int importMsgTtl = 0;
	// Verified but not in radio-browser: playing, awaiting a name before we save.
	bool needName = false;
	std::string pendingIcon;

	Radio() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		// Built-in VCA, identical to the core AUDIO module's LEVEL knob: 0–2 linear
		// param (default 1 = unity), shown as dB (−∞ … +12), gain = param².
		configParam(LEVEL_PARAM, 0.f, 2.f, 1.f, "Level", " dB", -10, 40);
		configInput(LEVEL_INPUT, "Level CV");
		configOutput(LEFT_OUTPUT, "Left");
		configOutput(RIGHT_OUTPUT, "Right");
		stream.setSampleRate(APP->engine->getSampleRate());
	}

	void play() {
		stream.start(url);
		playing = true;
	}
	void stopStream() {
		stream.stop();
		playing = false;
	}
	void togglePlay() {
		if (playing)
			stopStream();
		else
			play();
	}

	// Audition a URL: snapshot the current station, play the new URL, and kick the
	// background importer to verify real audio → identify → fetch art. Nothing is
	// committed or saved until the audition succeeds; a failure rolls back (see
	// ImportWatcher). Provisional name "Auditioning…" so the panel never claims the
	// new station prematurely.
	void auditionUrl(const std::string& u) {
		if (u.empty())
			return;
		prevUrl = url;
		prevName = stationName;
		prevIcon = icon;
		prevPlaying = playing;
		needName = false;
		importError = false;
		url = u;
		stationName = "Auditioning\xe2\x80\xa6";
		icon = "";
		play();
		setImportMsg("Auditioning\xe2\x80\xa6", false);
		importer.start(u, [this]() {
			akaudio::StationImporter::Probe p;
			p.state = (int) stream.getState();
			p.frames = stream.producedFrames();
			p.status = stream.getStatusText();
			return p;
		}, asset::user("akaudio-stations"));
	}

	// Undo a failed audition: restore the snapshot and its play state.
	void rollback() {
		url = prevUrl;
		stationName = prevName;
		icon = prevIcon;
		needName = false;
		stopStream();
		if (prevPlaying)
			play();
	}

	void setImportMsg(const std::string& msg, bool error) {
		importMsg = msg;
		importError = error;
		importMsgTtl = 60 * 6; // ~6 s at 60 fps
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		stream.setSampleRate(e.sampleRate);
	}

	void process(const ProcessArgs& args) override {
		float l = 0.f, r = 0.f;
		stream.pull(l, r); // leaves l/r at 0 on underrun
		// Built-in VCA: knob (exponential/audio taper) × optional CV (unipolar 0–10V).
		// Unpatched CV leaves the gain at the knob level.
		// gain = param² (AUDIO-module taper); optional unipolar 0–10 V CV scales it.
		float gain = std::pow(params[LEVEL_PARAM].getValue(), 2.f);
		if (inputs[LEVEL_INPUT].isConnected())
			gain *= clamp(inputs[LEVEL_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		// Audio in Rack is ±5V line level.
		outputs[LEFT_OUTPUT].setVoltage(l * 5.f * gain);
		outputs[RIGHT_OUTPUT].setVoltage(r * 5.f * gain);
		// The playing state is shown by the clickable LED (reads stream state).
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "url", json_string(url.c_str()));
		json_object_set_new(root, "stationName", json_string(stationName.c_str()));
		json_object_set_new(root, "icon", json_string(icon.c_str()));
		json_object_set_new(root, "playing", json_boolean(playing));
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "url"))
			url = json_string_value(j);
		if (json_t* j = json_object_get(root, "stationName"))
			stationName = json_string_value(j);
		if (json_t* j = json_object_get(root, "icon"))
			icon = json_string_value(j);
		// Loading a preset/patch: apply its playing state. Stop first so switching
		// to a "stopped" station preset actually stops current playback, and so
		// play() restarts cleanly on the new URL.
		bool wantPlay = false;
		if (json_t* j = json_object_get(root, "playing"))
			wantPlay = json_boolean_value(j);
		stopStream();
		if (wantPlay)
			play();
	}
};

// Editable URL field shown in the context menu. Entering a URL auditions it
// (verify real audio → identify → fetch art); see Radio::auditionUrl.
struct UrlField : ui::TextField {
	Radio* module = nullptr;
	void onAction(const ActionEvent& e) override {
		if (module && !text.empty())
			module->auditionUrl(text);
		ui::TextField::onAction(e);
	}
};

// Draw a bundled plugin image (png) into the rect, rounded, scaled to fill. Returns
// false (drawing nothing) if iconPath is empty or the image fails to load, so callers
// can fall back to a synthesized avatar. Cached by Rack.
static bool drawStationArt(NVGcontext* vg, const std::string& iconPath,
		float x, float y, float w, float h, float radius) {
	if (iconPath.empty())
		return false;
	// An absolute path = a runtime-cached favicon (importer); otherwise it's a
	// bundled plugin-relative path under res/.
	std::string file = iconPath[0] == '/'
		? iconPath : asset::plugin(pluginInstance, "res/" + iconPath);
	std::shared_ptr<window::Image> img = APP->window->loadImage(file);
	if (!img || img->handle < 0)
		return false;
	NVGpaint paint = nvgImagePattern(vg, x, y, w, h, 0.f, img->handle, 1.f);
	nvgBeginPath(vg);
	nvgRoundedRect(vg, x, y, w, h, radius);
	nvgFillPaint(vg, paint);
	nvgFill(vg);
	return true;
}

// Synthesize an avatar from the station name: 1-2 initials on a name-hashed color.
// Used when a station has no (usable) favicon, so we never ship hand-made art.
static void drawSynthIcon(NVGcontext* vg, float x, float y, float w, float h,
		const std::string& name, float radius) {
	// Initials: first letter of up to two words.
	std::string initials;
	bool atWordStart = true;
	for (char c : name) {
		if (std::isalnum((unsigned char) c)) {
			if (atWordStart) {
				initials += (char) std::toupper((unsigned char) c);
				if (initials.size() >= 2)
					break;
			}
			atWordStart = false;
		} else {
			atWordStart = true;
		}
	}
	if (initials.empty())
		initials = "?";
	// FNV-1a hash of the name -> palette index (stable per station).
	unsigned hash = 2166136261u;
	for (char c : name)
		hash = (hash ^ (unsigned char) c) * 16777619u;
	static const NVGcolor palette[] = {
		nvgRGB(0x3b, 0x6f, 0xc4), nvgRGB(0xc0, 0x39, 0x2b), nvgRGB(0x2e, 0x8b, 0x57),
		nvgRGB(0xd3, 0x7e, 0x1f), nvgRGB(0x7d, 0x3c, 0x98), nvgRGB(0x16, 0x8a, 0x7e),
		nvgRGB(0x33, 0x4a, 0x8a), nvgRGB(0xb0, 0x3a, 0x6e), nvgRGB(0x6d, 0x4c, 0x41),
		nvgRGB(0x44, 0x5a, 0x64), nvgRGB(0x2f, 0x7d, 0x32), nvgRGB(0xb5, 0x6a, 0x16),
	};
	NVGcolor bg = palette[hash % (sizeof(palette) / sizeof(palette[0]))];
	nvgBeginPath(vg);
	nvgRoundedRect(vg, x, y, w, h, radius);
	nvgFillColor(vg, bg);
	nvgFill(vg);
	radioText(vg, RADIO_FONT_BOLD, x + w / 2, y + h / 2 + 1.f, h * 0.40f,
		nvgRGBA(0xff, 0xff, 0xff, 0xee), initials.c_str());
}

// Strip a leading "NN_" ordering prefix (Rack's preset-sort convention).
static std::string prettyPresetName(std::string stem) {
	size_t us = stem.find('_');
	if (us != std::string::npos && us > 0) {
		bool allDigits = true;
		for (size_t i = 0; i < us; i++)
			if (!std::isdigit((unsigned char) stem[i])) {
				allDigits = false;
				break;
			}
		if (allDigits)
			return stem.substr(us + 1);
	}
	return stem;
}

// A station row in the picker menu: native menu item with the station's art
// thumbnail drawn at the right, and a green ring when it's the current station.
struct StationItem : MenuItem {
	std::string icon;
	bool current = false;
	std::function<void()> onSelect;

	void onAction(const ActionEvent& e) override {
		if (onSelect)
			onSelect();
	}
	void step() override {
		MenuItem::step();
		box.size.y = std::max(box.size.y, 22.f);
		box.size.x += 28.f; // reserve space for the thumbnail on the right
	}
	void draw(const DrawArgs& args) override {
		MenuItem::draw(args); // native highlight + left-aligned text
		const float s = box.size.y - 6.f;
		const float ix = box.size.x - s - 5.f, iy = 3.f;
		if (!drawStationArt(args.vg, icon, ix, iy, s, s, 3.f))
			drawSynthIcon(args.vg, ix, iy, s, s, text, 3.f);
		if (current) {
			nvgBeginPath(args.vg);
			nvgRoundedRect(args.vg, ix - 1.f, iy - 1.f, s + 2.f, s + 2.f, 4.f);
			nvgStrokeColor(args.vg, nvgRGB(0x3a, 0xd0, 0x6a));
			nvgStrokeWidth(args.vg, 1.5f);
			nvgStroke(args.vg);
		}
	}
};

// Read a station preset's display name + icon from its .vcvm (data block).
static void readStationPreset(const std::string& path, std::string& name, std::string& icon) {
	name.clear();
	icon.clear();
	json_error_t err;
	json_t* root = json_load_file(path.c_str(), 0, &err);
	if (!root)
		return;
	if (json_t* data = json_object_get(root, "data")) {
		if (json_t* j = json_object_get(data, "stationName"))
			name = json_string_value(j);
		if (json_t* j = json_object_get(data, "icon"))
			icon = json_string_value(j);
	}
	json_decref(root);
}

// --- Saving auditioned stations (dedup by URL) ------------------------------

// Read a preset's data.url, or "" if absent.
static std::string readPresetUrl(const std::string& path) {
	json_error_t err;
	json_t* root = json_load_file(path.c_str(), 0, &err);
	if (!root)
		return "";
	std::string u;
	if (json_t* data = json_object_get(root, "data"))
		if (json_t* j = json_object_get(data, "url"))
			if (json_is_string(j))
				u = json_string_value(j);
	json_decref(root);
	return u;
}

// Does any .vcvm under dir (recursively) already carry this URL? Used to avoid
// duplicating a bundled (factory) station.
static bool dirHasPresetUrl(const std::string& dir, const std::string& url) {
	if (url.empty() || !system::isDirectory(dir))
		return false;
	for (const std::string& path : system::getEntries(dir)) {
		if (system::isDirectory(path)) {
			if (dirHasPresetUrl(path, url))
				return true;
		} else if (system::getExtension(path) == ".vcvm" && readPresetUrl(path) == url) {
			return true;
		}
	}
	return false;
}

// A user preset (non-recursive) with this URL, for overwrite-in-place; "" if none.
static std::string userPresetWithUrl(const std::string& dir, const std::string& url) {
	if (url.empty() || !system::isDirectory(dir))
		return "";
	for (const std::string& path : system::getEntries(dir))
		if (system::getExtension(path) == ".vcvm" && readPresetUrl(path) == url)
			return path;
	return "";
}

static std::string sanitizeName(const std::string& name) {
	std::string out;
	for (char c : name)
		out += (c == '/' || c == ':' || c == '\\') ? '-' : c;
	if (out.empty())
		out = "Station";
	return out;
}

// Write a Radio station preset (data = url/name/icon, playing:true).
static void writeStationPreset(const std::string& path, const std::string& url,
		const std::string& name, const std::string& icon) {
	json_t* root = json_object();
	json_object_set_new(root, "plugin", json_string(pluginInstance->slug.c_str()));
	json_object_set_new(root, "model", json_string("Radio"));
	json_object_set_new(root, "version", json_string(pluginInstance->version.c_str()));
	json_object_set_new(root, "params", json_array());
	json_t* data = json_object();
	json_object_set_new(data, "url", json_string(url.c_str()));
	json_object_set_new(data, "stationName", json_string(name.c_str()));
	json_object_set_new(data, "icon", json_string(icon.c_str()));
	json_object_set_new(data, "playing", json_true());
	json_object_set_new(root, "data", data);
	json_dump_file(root, path.c_str(), JSON_INDENT(2));
	json_decref(root);
}

// Save a station to the user preset dir, deduped: skip if a bundled station
// already has this URL; overwrite an existing user preset with the same URL;
// otherwise create a new one. Returns a short outcome for the status line.
static std::string saveUserStation(Module* module, const std::string& name,
		const std::string& url, const std::string& icon) {
	if (!module || !module->model || url.empty())
		return "";
	if (dirHasPresetUrl(module->model->getFactoryPresetDirectory(), url))
		return "Already in Stations";
	std::string dir = module->model->getUserPresetDirectory();
	system::createDirectories(dir);
	std::string existing = userPresetWithUrl(dir, url);
	std::string path = existing.empty() ? (dir + "/" + sanitizeName(name) + ".vcvm") : existing;
	writeStationPreset(path, url, name, icon);
	return existing.empty() ? "Added to Your stations" : "Updated station";
}

// Collect station preset paths in the same order the picker menu shows them
// (subfolders' contents first, then loose files, each sorted), for prev/next
// stepping.
static void collectStationDir(const std::string& dir, std::vector<std::string>& out) {
	if (!system::isDirectory(dir))
		return;
	std::vector<std::string> entries = system::getEntries(dir);
	std::sort(entries.begin(), entries.end());
	for (const std::string& p : entries)
		if (system::isDirectory(p))
			collectStationDir(p, out);
	for (const std::string& p : entries)
		if (system::getExtension(p) == ".vcvm")
			out.push_back(p);
}

// Flat ordered list of all stations: bundled (factory) then user-saved. Deduped
// by URL (keeping the first), so a duplicate station — e.g. a stale leftover
// preset folder — can't make prev/next bounce within one region instead of
// advancing.
static std::vector<std::string> collectStations(ModuleWidget* mw) {
	std::vector<std::string> raw;
	collectStationDir(mw->model->getFactoryPresetDirectory(), raw);
	collectStationDir(mw->model->getUserPresetDirectory(), raw);

	std::vector<std::string> out;
	std::set<std::string> seenUrls;
	for (const std::string& path : raw) {
		std::string u = readPresetUrl(path);
		if (!u.empty() && !seenUrls.insert(u).second)
			continue; // a preset with this URL is already in the list
		out.push_back(path);
	}
	return out;
}

// Recursively populate a menu from a station directory: subfolders (= a
// provider/category, e.g. "Nature") become submenus, .vcvm files become
// station rows. Mirrors Rack's own factory-preset folder convention, so the
// native Preset menu groups identically.
static void appendStationDir(Menu* menu, ModuleWidget* mw, const std::string& dir) {
	Radio* module = dynamic_cast<Radio*>(mw->module);
	std::vector<std::string> entries = system::getEntries(dir);
	std::sort(entries.begin(), entries.end());
	int count = 0;

	// Folders first, as submenus.
	for (const std::string& path : entries) {
		if (!system::isDirectory(path))
			continue;
		std::string label = prettyPresetName(system::getFilename(path));
		menu->addChild(createSubmenuItem(label, "", [mw, path](Menu* sub) {
			appendStationDir(sub, mw, path);
		}));
		count++;
	}
	// Then station presets.
	for (const std::string& path : entries) {
		if (system::getExtension(path) != ".vcvm")
			continue;
		std::string name, icon;
		readStationPreset(path, name, icon);
		if (name.empty())
			name = prettyPresetName(system::getStem(path));

		StationItem* item = new StationItem;
		item->text = name;
		item->icon = icon;
		item->current = module && module->stationName == name;
		WeakPtr<ModuleWidget> w = mw;
		item->onSelect = [w, path]() { if (w) w->loadAction(path); };
		menu->addChild(item);
		count++;
	}
	if (count == 0)
		menu->addChild(createMenuLabel("No stations"));
}

// Bundled stations = factory presets in presets/Radio/, grouped by subfolder; plus any
// stations the user has saved (user preset dir, shown under "Your stations").
static void appendStationMenu(Menu* menu, ModuleWidget* mw) {
	std::string dir = mw->model->getFactoryPresetDirectory();
	if (system::isDirectory(dir))
		appendStationDir(menu, mw, dir);
	else
		menu->addChild(createMenuLabel("No stations bundled"));
	std::string userDir = mw->model->getUserPresetDirectory();
	if (system::isDirectory(userDir) && !system::getEntries(userDir).empty()) {
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Your stations"));
		appendStationDir(menu, mw, userDir);
	}
}

// (User stations are now created automatically by the importer when a URL is
// entered — see Radio::importStation and net/StationImport — so there is no
// manual save-by-name path here.)

// On-panel station artwork (the current station's bundled logo).
struct StationArt : Widget {
	Radio* module = nullptr;
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		const float w = box.size.x, h = box.size.y;
		const std::string name = module ? module->stationName : "";
		// Backing (shows through transparent favicons).
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, w, h, 5);
		nvgFillColor(vg, nvgRGBA(0, 0, 0, 0x22));
		nvgFill(vg);
		bool drew = module && drawStationArt(vg, module->icon, 0, 0, w, h, 5);
		if (!drew) {
			if (name.empty()) {
				// No station picked yet: faint ♪ placeholder.
				std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
				if (font && font->handle >= 0) {
					nvgFontFaceId(vg, font->handle);
					nvgFontSize(vg, h * 0.5f);
					nvgFillColor(vg, nvgRGBA(0x24, 0x27, 0x2b, 0x55));
					nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
					nvgText(vg, w / 2, h / 2, "\xe2\x99\xaa", NULL); // ♪
				}
			} else {
				// Station with no usable favicon: synthesize an avatar from its name.
				drawSynthIcon(vg, 0, 0, w, h, name, 5);
			}
		}
	}
};

// On-panel station picker: shows the current station, opens the stations menu.
struct StationChoice : LedDisplayChoice {
	Radio* module = nullptr;
	ModuleWidget* mw = nullptr;

	void step() override {
		text = (module && !module->stationName.empty()) ? module->stationName : "Pick a station\xe2\x80\xa6";
		LedDisplayChoice::step();
	}
	void onAction(const ActionEvent& e) override {
		if (!mw)
			return;
		ui::Menu* menu = createMenu();
		menu->addChild(createMenuLabel("Stations"));
		appendStationMenu(menu, mw);
	}
};

// Invisible helper (UI thread): drives the audition outcome — applies the live
// importer status, then on completion either commits + saves (identified),
// awaits a name (verified-but-unknown), or rolls back (failed). All saving and
// rollback are centralised here so no failed audition leaves lingering state.
struct ImportWatcher : Widget {
	Radio* module = nullptr;
	unsigned lastGen = 0;
	bool inited = false;

	void step() override {
		if (module) {
			// On first appearance, sync to the current generation so a stale result
			// from before this widget existed isn't re-applied (e.g. patch reopen).
			if (!inited) {
				inited = true;
				lastGen = module->importer.generation();
			}

			// Live status while a run is in flight; otherwise count the outcome down.
			if (module->importer.running()) {
				module->importMsg = module->importer.status();
				module->importError = false;
				module->importMsgTtl = 60 * 6;
			} else if (module->importMsgTtl > 0 && --module->importMsgTtl == 0) {
				module->importMsg = "";
			}

			unsigned g = module->importer.generation();
			if (g != lastGen) {
				lastGen = g;
				akaudio::StationImporter::Result r = module->importer.result();
				// Ignore if the user has navigated away from the auditioned URL.
				if (r.url == module->url) {
					if (!r.ok) {
						module->setImportMsg("\xe2\x9c\x95 " + r.status, true); // ✕ reason
						module->rollback();
					} else if (r.identified) {
						module->stationName = r.name;
						module->icon = r.iconPath;
						module->needName = false;
						module->setImportMsg(saveUserStation(module, r.name, r.url, r.iconPath), false);
					} else {
						// Verified but unknown: play it, await a name to save.
						module->stationName = "Unknown station";
						module->icon = r.iconPath;
						module->pendingIcon = r.iconPath;
						module->needName = true;
						module->setImportMsg(r.status, false);
					}
				}
			}
		}
		Widget::step();
	}
};

// On-panel status line under the picker: shows the audition status ("Auditioning…",
// "Added …") or, in red, a clear failure ("✕ Connected, but no audio").
struct StatusLine : Widget {
	Radio* module = nullptr;
	void draw(const DrawArgs& args) override {
		if (!module || module->importMsg.empty())
			return;
		NVGcolor col = module->importError ? nvgRGB(0xc0, 0x39, 0x2b) : nvgRGB(0x2e, 0x7d, 0x46);
		nvgScissor(args.vg, 0, 0, box.size.x, box.size.y);
		radioText(args.vg, RADIO_FONT_BOLD, box.size.x / 2, box.size.y / 2, 9.5f,
			col, module->importMsg.c_str());
		nvgResetScissor(args.vg);
	}
};

// Name field for a verified-but-unknown station: Enter saves it under that name.
struct NameField : ui::TextField {
	Radio* module = nullptr;
	NameField() { placeholder = "Name this station\xe2\x80\xa6"; }
	void onAction(const ActionEvent& e) override {
		if (module && !text.empty() && !module->url.empty()) {
			module->stationName = text;
			module->needName = false;
			module->setImportMsg(saveUserStation(module, text, module->url, module->pendingIcon), false);
		}
		ui::TextField::onAction(e);
	}
};

// Small triangular up/down button to step to the previous/next station.
struct StepButton : OpaqueWidget {
	bool up = true;
	bool hovered = false;
	std::function<void()> onStep;

	void onEnter(const EnterEvent& e) override { hovered = true; OpaqueWidget::onEnter(e); }
	void onLeave(const LeaveEvent& e) override { hovered = false; OpaqueWidget::onLeave(e); }
	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && onStep) {
			onStep();
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
	}
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		const float w = box.size.x, h = box.size.y;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, w, h, 2.f);
		nvgFillColor(vg, hovered ? nvgRGBA(0, 0, 0, 0x1e) : nvgRGBA(0, 0, 0, 0x12));
		nvgFill(vg);
		// Centered triangle (up or down).
		const float cx = w / 2, cy = h / 2, tw = w * 0.24f, th = h * 0.20f;
		nvgBeginPath(vg);
		if (up) {
			nvgMoveTo(vg, cx, cy - th);
			nvgLineTo(vg, cx - tw, cy + th);
			nvgLineTo(vg, cx + tw, cy + th);
		} else {
			nvgMoveTo(vg, cx, cy + th);
			nvgLineTo(vg, cx - tw, cy - th);
			nvgLineTo(vg, cx + tw, cy - th);
		}
		nvgClosePath(vg);
		nvgFillColor(vg, hovered ? RADIO_TEXT : RADIO_TEXT_DIM);
		nvgFill(vg);
	}
};

struct RadioWidget : ModuleWidget {
	// Load the previous/next station preset (bundled, then user), matching the
	// current one by URL; wraps around. Uses Rack's preset loader, so it plays
	// the station with full undo.
	void stepStation(int dir) {
		std::vector<std::string> paths = collectStations(this);
		if (paths.empty())
			return;
		Radio* m = getModule<Radio>();
		std::string curUrl = m ? m->url : "";
		int idx = -1;
		for (int i = 0; i < (int) paths.size(); i++)
			if (readPresetUrl(paths[i]) == curUrl) {
				idx = i;
				break;
			}
		int n = (int) paths.size();
		int next = (idx < 0) ? (dir > 0 ? 0 : n - 1) : (((idx + dir) % n) + n) % n;
		loadAction(paths[next]);
	}

	RadioWidget(Radio* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Radio.svg")));

		// Code-drawn title + L/R labels (NanoSVG won't render <text>).
		RadioDecor* decor = new RadioDecor;
		decor->box.size = box.size;
		addChild(decor);

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Station artwork (visual identification) — a logo square on the left; the
		// right strip holds the status LED + prev/next stepper.
		StationArt* art = new StationArt;
		art->module = module;
		art->box.pos = mm2px(Vec(4.0, 18.0));
		art->box.size = mm2px(Vec(22.0, 22.0));
		addChild(art);

		// Clickable status LED at the top of the right strip beside the artwork:
		// green=playing, amber=connecting, red=stopped. Click toggles playback.
		ClickableLed* led = new ClickableLed;
		led->box.size = mm2px(Vec(5.0, 5.0));
		led->box.pos = mm2px(Vec(31.14, 19.75));
		// Green only when audio is *actually flowing* (Playing AND frames decoded),
		// so a connected-but-silent stream reads as pending (amber), not live.
		auto reallyLive = [](Radio* m) {
			return m && m->stream.getState() == akaudio::StreamClient::State::Playing
				&& m->stream.producedFrames() > 0;
		};
		led->isLive = [module, reallyLive]() { return reallyLive(module); };
		led->isPending = [module, reallyLive]() { return module && module->playing && !reallyLive(module); };
		led->onToggle = [module]() { if (module) module->togglePlay(); };
		addChild(led);

		// On-panel station picker (click to choose a bundled station). Dark text on a
		// subtle recessed field, matching the silver Fundamental theme. Near full width
		// so long station names fit.
		StationChoice* choice = new StationChoice;
		choice->module = module;
		choice->mw = this;
		choice->color = RADIO_TEXT;
		choice->bgColor = nvgRGBA(0, 0, 0, 0x14);
		choice->fontPath = asset::system("res/fonts/DejaVuSans.ttf");
		choice->box.pos = mm2px(Vec(3.0, 46.0));
		choice->box.size = mm2px(Vec(34.64, 8.5));
		addChild(choice);

		// Prev/next station stepper in the right strip beside the artwork, below the
		// LED (up = previous, down = next).
		StepButton* up = new StepButton;
		up->up = true;
		up->box.pos = mm2px(Vec(30.64, 26.5));
		up->box.size = mm2px(Vec(6.0, 5.0));
		up->onStep = [this]() { stepStation(-1); };
		addChild(up);

		StepButton* down = new StepButton;
		down->up = false;
		down->box.pos = mm2px(Vec(30.64, 33.25));
		down->box.size = mm2px(Vec(6.0, 5.0));
		down->onStep = [this]() { stepStation(1); };
		addChild(down);

		// Built-in VCA: the AUDIO module's LEVEL knob (RoundLargeBlackKnob), with an
		// optional CV cable below. "LEVEL"/"CV"/"−∞"/"+12" are drawn by RadioDecor.
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(20.32, 77.362)), module, Radio::LEVEL_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.32, 96.859)), module, Radio::LEVEL_INPUT));

		// Stereo outputs on the dark plate (drawn by RadioDecor; LEFT/RIGHT labels too).
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(OUT_L_X, OUT_Y)), module, Radio::LEFT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(OUT_R_X, OUT_Y)), module, Radio::RIGHT_OUTPUT));

		// Audition status line in the gap under the picker (empty when idle).
		StatusLine* status = new StatusLine;
		status->module = module;
		status->box.pos = mm2px(Vec(2.5, 55.5));
		status->box.size = mm2px(Vec(35.64, 7.0));
		addChild(status);

		// Drives audition outcomes (commit/save/rollback) on the UI thread.
		ImportWatcher* watcher = new ImportWatcher;
		watcher->module = module;
		addChild(watcher);
	}

	void appendContextMenu(Menu* menu) override {
		// Strip Rack's two non-functional leading labels (model name "Radio" +
		// brand "AK Audio"). They're the only leading MenuLabels createContextMenu
		// adds before this hook; the next item (Info) is a MenuItem, which stops us.
		while (!menu->children.empty()) {
			ui::MenuLabel* label = dynamic_cast<ui::MenuLabel*>(menu->children.front());
			if (!label)
				break;
			menu->removeChild(label);
			delete label;
		}

		Radio* module = getModule<Radio>();
		if (!module)
			return;

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Status: " + module->stream.getStatusText()));

		menu->addChild(createMenuItem(module->playing ? "Stop" : "Play", "", [module]() {
			module->togglePlay();
		}));

		// No "Stations" submenu here: the on-panel picker (StationChoice) is the
		// nice one (artwork + ✓current), and Rack's native "Preset ▸" menu already
		// lists the same factory presets grouped by category — so a context-menu
		// copy would just duplicate "Preset ▸".

		// Paste a stream URL + Enter: auditions it (verify real audio → identify →
		// fetch art). Identified stations are saved automatically; an unknown
		// stream plays and asks for a name below before it's saved.
		menu->addChild(createMenuLabel("Stream URL \xe2\x80\x94 Enter to audition:"));
		UrlField* field = new UrlField;
		field->box.size.x = 260;
		field->module = module;
		field->text = module->url;
		field->placeholder = "http(s)://host/path  (MP3 / AAC / HLS)";
		menu->addChild(field);

		// Live audition status (verifying / saved / failure reason).
		if (!module->importMsg.empty())
			menu->addChild(createMenuLabel(module->importMsg));

		// Verified-but-unknown stream: name it to save under "Your stations".
		if (module->needName) {
			NameField* nf = new NameField;
			nf->box.size.x = 260;
			nf->module = module;
			menu->addChild(nf);
		}

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Find more stations \xe2\x80\x94 radio-browser.info \xe2\x86\x97", "",
			[]() { system::openBrowser("https://www.radio-browser.info/"); }));
	}
};

Model* modelRadio = createModel<Radio, RadioWidget>("Radio");
