#include "plugin.hpp"
#include "net/Stream.hpp"

#include <algorithm>
#include <cctype>

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
	std::string url = "http://ice1.somafm.com/groovesalad-128-mp3";
	// Human label for the current station (shown on the panel, persisted in
	// presets). "Stations" are just curated factory presets — see presets/Radio/.
	std::string stationName = "SomaFM Groove Salad";
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
		json_object_set_new(root, "playing", json_boolean(playing));
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "url"))
			url = json_string_value(j);
		if (json_t* j = json_object_get(root, "stationName"))
			stationName = json_string_value(j);
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
		}
		ui::TextField::onAction(e);
	}
};

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

// Populate a menu with the bundled stations (= factory presets in
// presets/Radio/), loading the chosen one through Rack's own preset loader.
static void appendStationMenu(Menu* menu, ModuleWidget* mw) {
	std::string dir = mw->model->getFactoryPresetDirectory();
	if (!system::isDirectory(dir)) {
		menu->addChild(createMenuLabel("No stations bundled"));
		return;
	}
	std::vector<std::string> entries = system::getEntries(dir);
	std::sort(entries.begin(), entries.end());

	Radio* module = dynamic_cast<Radio*>(mw->module);
	int count = 0;
	for (const std::string& path : entries) {
		if (system::getExtension(path) != ".vcvm")
			continue;
		std::string name = prettyPresetName(system::getStem(path));
		bool current = module && module->stationName == name;
		WeakPtr<ModuleWidget> w = mw;
		menu->addChild(createCheckMenuItem(name, "",
			[current]() { return current; },
			[w, path]() { if (w) w->loadAction(path); }));
		count++;
	}
	if (count == 0)
		menu->addChild(createMenuLabel("No stations bundled"));
}

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

		// On-panel station picker (click to choose a bundled station).
		StationChoice* choice = new StationChoice;
		choice->module = module;
		choice->mw = this;
		choice->box.pos = mm2px(Vec(2.5, 14.0));
		choice->box.size = mm2px(Vec(35.64, 8.0));
		addChild(choice);

		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(20.32, 32.0)), module, Radio::PLAYING_LIGHT));

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

		menu->addChild(createSubmenuItem("Stations", module->stationName, [this](Menu* sub) {
			appendStationMenu(sub, this);
		}));

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
