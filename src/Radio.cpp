#include "plugin.hpp"
#include "net/Stream.hpp"

#include <algorithm>
#include <cctype>
#include <functional>

// Streaming internet radio (Icecast/HTTP MP3) source. Audio is fetched and decoded
// on a background thread (net/Stream.hpp) and pulled here on the audio thread.
struct Radio : Module {
	enum ParamId {
		PARAMS_LEN
	};
	enum InputId {
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

	akozlov::StreamClient stream;
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
		// Audio in Rack is ±5V line level.
		outputs[LEFT_OUTPUT].setVoltage(l * 5.f);
		outputs[RIGHT_OUTPUT].setVoltage(r * 5.f);

		bool live = stream.getState() == akozlov::StreamClient::State::Playing;
		lights[PLAYING_LIGHT].setBrightnessSmooth(live ? 1.f : 0.f, args.sampleTime);
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

// Draw a bundled plugin image (png) into the rect, rounded, scaled to fill.
// No-op if iconPath is empty or the image fails to load. Cached by Rack.
static void drawStationArt(NVGcontext* vg, const std::string& iconPath,
		float x, float y, float w, float h, float radius) {
	if (iconPath.empty())
		return;
	std::shared_ptr<window::Image> img =
		APP->window->loadImage(asset::plugin(pluginInstance, "res/" + iconPath));
	if (!img || img->handle < 0)
		return;
	NVGpaint paint = nvgImagePattern(vg, x, y, w, h, 0.f, img->handle, 1.f);
	nvgBeginPath(vg);
	nvgRoundedRect(vg, x, y, w, h, radius);
	nvgFillPaint(vg, paint);
	nvgFill(vg);
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
		drawStationArt(args.vg, icon, ix, iy, s, s, 3.f);
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

// Bundled stations = factory presets in presets/Radio/, grouped by subfolder.
static void appendStationMenu(Menu* menu, ModuleWidget* mw) {
	std::string dir = mw->model->getFactoryPresetDirectory();
	if (!system::isDirectory(dir)) {
		menu->addChild(createMenuLabel("No stations bundled"));
		return;
	}
	appendStationDir(menu, mw, dir);
}

// On-panel station artwork (the current station's bundled logo).
struct StationArt : Widget {
	Radio* module = nullptr;
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		const float w = box.size.x, h = box.size.y;
		// Recessed frame.
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, w, h, 5);
		nvgFillColor(vg, nvgRGBA(0, 0, 0, 0x66));
		nvgFill(vg);
		if (module && !module->icon.empty()) {
			drawStationArt(vg, module->icon, 0, 0, w, h, 5);
		}
		else {
			// Placeholder ♪ when there's no art (e.g. a custom URL).
			std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
			if (font && font->handle >= 0) {
				nvgFontFaceId(vg, font->handle);
				nvgFontSize(vg, h * 0.5f);
				nvgFillColor(vg, nvgRGBA(0xff, 0xff, 0xff, 0x33));
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgText(vg, w / 2, h / 2, "\xe2\x99\xaa", NULL); // ♪
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

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Station artwork (visual identification).
		StationArt* art = new StationArt;
		art->module = module;
		art->box.pos = mm2px(Vec(5.32, 11.0));
		art->box.size = mm2px(Vec(30.0, 30.0));
		addChild(art);

		// On-panel station picker (click to choose a bundled station).
		StationChoice* choice = new StationChoice;
		choice->module = module;
		choice->mw = this;
		choice->box.pos = mm2px(Vec(2.5, 44.0));
		choice->box.size = mm2px(Vec(35.64, 8.0));
		addChild(choice);

		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(20.32, 56.0)), module, Radio::PLAYING_LIGHT));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(13.0, 100.0)), module, Radio::LEFT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(27.0, 100.0)), module, Radio::RIGHT_OUTPUT));
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
	}
};

Model* modelRadio = createModel<Radio, RadioWidget>("Radio");
