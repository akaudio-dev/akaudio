#include "plugin.hpp"
#include "net/Stream.hpp"
#include "ClickableLed.hpp"

#include <algorithm>
#include <cctype>
#include <functional>

// Fundamental-style theme (matches the Ninjam panel): silver panel, dark labels.
static const char* RADIO_FONT_BOLD = "res/fonts/Nunito-Bold.ttf";
static const NVGcolor RADIO_TEXT     = nvgRGB(0x24, 0x27, 0x2b); // primary dark text
static const NVGcolor RADIO_TEXT_DIM = nvgRGB(0x6a, 0x72, 0x7a); // secondary labels
static const NVGcolor RADIO_TEXT_LT  = nvgRGB(0xe8, 0xea, 0xec); // labels on the dark output plate
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

// Panel title ("RADIO") in the header strip + L/R labels above the jacks. Drawn in code
// because Rack's panel renderer (NanoSVG) ignores <text>.
struct RadioDecor : Widget {
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		// Black output plate (Fundamental convention: outputs sit on a dark #1F1F1F
		// rounded panel; inputs stay on the bare panel). Same width as the artwork
		// above for a tidy column. Drawn here (a low-z child) so the jacks, added
		// later, render on top of it.
		nvgBeginPath(vg);
		nvgRoundedRect(vg, mm2px(6.32f), mm2px(103.0f), mm2px(28.0f), mm2px(17.5f), mm2px(1.6f));
		nvgFillColor(vg, RADIO_PLATE);
		nvgFill(vg);

		radioText(vg, RADIO_FONT_BOLD, mm2px(20.32f), mm2px(7.2f), 13.f, RADIO_TEXT, "RADIO");
		radioText(vg, RADIO_FONT_BOLD, mm2px(20.32f), mm2px(65.0f), 11.f, RADIO_TEXT, "VOLUME");
		radioText(vg, RADIO_FONT_BOLD, mm2px(20.32f), mm2px(86.0f), 10.f, RADIO_TEXT_DIM, "CV");
		radioText(vg, RADIO_FONT_BOLD, mm2px(13.0f), mm2px(106.5f), 11.f, RADIO_TEXT_LT, "L");
		radioText(vg, RADIO_FONT_BOLD, mm2px(27.0f), mm2px(106.5f), 11.f, RADIO_TEXT_LT, "R");
		Widget::draw(args);
	}
};

// Volume knob taper. The param p ∈ [0,1] is the knob position; the applied gain is
// exponential in p (a proper audio/log taper): gain = 2000^p · 0.001, i.e. linear in
// dB. p=0 → 0.1 % (≈ −60 dB, effectively silent), default p≈0.909 → unity (100 %),
// p=1 → 2× (+6 dB, 200 %). We let Rack render this natively as a percentage via the
// configParam displayBase/displayMultiplier (display = 2000^p · 0.1), so there is no
// custom ParamQuantity — the knob behaves exactly like a stock Fundamental knob.
static const float VOL_DISPLAY_BASE = 2000.f;
static const float VOL_UNITY_POS = 0.90876f; // log(1000)/log(2000): p where gain == 1
static inline float radioGain(float p) { return std::pow(VOL_DISPLAY_BASE, p) * 0.001f; }

// Streaming internet radio (Icecast/HTTP MP3) source. Audio is fetched and decoded
// on a background thread (net/Stream.hpp) and pulled here on the audio thread.
struct Radio : Module {
	enum ParamId {
		VOLUME_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		VOLUME_INPUT, // CV modulation for the built-in VCA (unipolar 0–10V)
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
	// Default to a calm ambient bed; "stations" are curated factory presets
	// (sound sources for soundscapes — nature, scanners, space — not music).
	std::string url = "https://nature-rex.radioca.st/stream";
	// Human label for the current station (shown on the panel, persisted in
	// presets). See presets/Radio/ for the grouped set.
	std::string stationName = "Ambi Nature Radio";
	// Plugin-relative path to the station's bundled artwork (e.g.
	// "stations/ambinature.png"), shown on the panel. Empty = ♪ placeholder.
	std::string icon = "stations/ambinature.png";
	bool playing = false;

	Radio() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		// Built-in VCA: default unity gain (100 %), so existing patches are unchanged;
		// full right is +6 dB (200 %), full left is ~silence. Native % display.
		configParam(VOLUME_PARAM, 0.f, 1.f, VOL_UNITY_POS, "Volume", "%", VOL_DISPLAY_BASE, 0.1f);
		configInput(VOLUME_INPUT, "Volume CV");
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

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		stream.setSampleRate(e.sampleRate);
	}

	void process(const ProcessArgs& args) override {
		float l = 0.f, r = 0.f;
		stream.pull(l, r); // leaves l/r at 0 on underrun
		// Built-in VCA: knob (exponential/audio taper) × optional CV (unipolar 0–10V).
		// Unpatched CV leaves the gain at the knob level.
		float gain = radioGain(params[VOLUME_PARAM].getValue());
		if (inputs[VOLUME_INPUT].isConnected())
			gain *= clamp(inputs[VOLUME_INPUT].getVoltage() / 10.f, 0.f, 1.f);
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

// Editable URL field shown in the context menu.
struct UrlField : ui::TextField {
	Radio* module = nullptr;
	void onAction(const ActionEvent& e) override {
		if (module) {
			module->url = text;
			module->stationName = "Custom";
			module->icon = ""; // no bundled art for a hand-typed URL
		}
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
	std::shared_ptr<window::Image> img =
		APP->window->loadImage(asset::plugin(pluginInstance, "res/" + iconPath));
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

// Save the current URL as a user station (a preset in the user preset dir, so it shows
// under "Your stations" and Rack's native Preset menu). Icon is left empty -> synthesized.
static void saveUserStation(ModuleWidget* mw, const std::string& name, const std::string& url) {
	if (name.empty() || url.empty())
		return;
	std::string dir = mw->model->getUserPresetDirectory();
	system::createDirectories(dir);
	json_t* root = json_object();
	json_object_set_new(root, "plugin", json_string("akaudio"));
	json_object_set_new(root, "model", json_string("Radio"));
	json_object_set_new(root, "version", json_string("2.0.0"));
	json_object_set_new(root, "params", json_array());
	json_t* data = json_object();
	json_object_set_new(data, "url", json_string(url.c_str()));
	json_object_set_new(data, "stationName", json_string(name.c_str()));
	json_object_set_new(data, "icon", json_string(""));
	json_object_set_new(data, "playing", json_true());
	json_object_set_new(root, "data", data);
	std::string safe;
	for (char c : name)
		safe += (c == '/' || c == ':' || c == '\\') ? '-' : c;
	json_dump_file(root, (dir + "/" + safe + ".vcvm").c_str(), JSON_INDENT(2));
	json_decref(root);
}

// Name field in the "Add station" submenu: Enter saves the current URL under this name.
struct StationNameField : ui::TextField {
	ModuleWidget* mw = nullptr;
	Radio* module = nullptr;
	void onAction(const ActionEvent& e) override {
		if (mw && module && !text.empty() && !module->url.empty()) {
			saveUserStation(mw, text, module->url);
			module->stationName = text; // reflect on panel
		}
		ui::TextField::onAction(e);
	}
};

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

struct RadioWidget : ModuleWidget {
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

		// Station artwork (visual identification) — large square below the header.
		StationArt* art = new StationArt;
		art->module = module;
		art->box.pos = mm2px(Vec(6.32, 14.5));
		art->box.size = mm2px(Vec(28.0, 28.0));
		addChild(art);

		// Clickable status LED, as a "live" badge on the artwork's bottom-right corner:
		// green=playing, amber=connecting, red=stopped. Click toggles playback.
		ClickableLed* led = new ClickableLed;
		led->box.size = mm2px(Vec(5.0, 5.0));
		led->box.pos = mm2px(Vec(31.0, 38.0)).minus(led->box.size.div(2));
		led->isLive = [module]() { return module && module->stream.getState() == akaudio::StreamClient::State::Playing; };
		led->isPending = [module]() { return module && module->playing && module->stream.getState() != akaudio::StreamClient::State::Playing; };
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

		// Built-in VCA: a stock Fundamental RoundBlackKnob (centered) modulated by an
		// optional CV cable below it. Labels "VOLUME"/"CV" are drawn by RadioDecor.
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(20.32, 75.0)), module, Radio::VOLUME_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.32, 93.0)), module, Radio::VOLUME_INPUT));

		// Stereo outputs on the bottom row (Fundamental's y=113.115), on the black
		// plate drawn by RadioDecor; L/R labels also drawn there.
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(13.0, 113.115)), module, Radio::LEFT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(27.0, 113.115)), module, Radio::RIGHT_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
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

		menu->addChild(createMenuLabel("Stream URL (http(s):// MP3 or AAC):"));
		UrlField* field = new UrlField;
		field->box.size.x = 260;
		field->module = module;
		field->text = module->url;
		field->placeholder = "http://host:port/path";
		menu->addChild(field);

		// --- Add your own stations ---
		menu->addChild(new MenuSeparator);
		ModuleWidget* mw = this;
		menu->addChild(createSubmenuItem("Save current URL as a station\xe2\x80\xa6", "",
			[mw, module](Menu* sub) {
				if (module->url.empty()) {
					sub->addChild(createMenuLabel("Set a stream URL first"));
					return;
				}
				sub->addChild(createMenuLabel("Station name (Enter to save):"));
				StationNameField* nf = new StationNameField;
				nf->box.size.x = 200;
				nf->mw = mw;
				nf->module = module;
				nf->text = module->stationName == "Custom" ? "" : module->stationName;
				nf->placeholder = "My station";
				sub->addChild(nf);
			}));
		menu->addChild(createMenuItem("Find more stations \xe2\x80\x94 radio-browser.info \xe2\x86\x97", "",
			[]() { system::openBrowser("https://www.radio-browser.info/"); }));
	}
};

Model* modelRadio = createModel<Radio, RadioWidget>("Radio");
