#include "plugin.hpp"
#include "net/Stream.hpp"

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
		json_object_set_new(root, "playing", json_boolean(playing));
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "url"))
			url = json_string_value(j);
		if (json_t* j = json_object_get(root, "playing"))
			playing = json_boolean_value(j);
		if (playing)
			play();
	}
};

// Editable URL field shown in the context menu.
struct UrlField : ui::TextField {
	Radio* module = nullptr;
	void onAction(const ActionEvent& e) override {
		if (module)
			module->url = text;
		ui::TextField::onAction(e);
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

		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(20.32, 30.0)), module, Radio::PLAYING_LIGHT));

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

		menu->addChild(createMenuLabel("Stream URL (http:// MP3):"));
		UrlField* field = new UrlField;
		field->box.size.x = 260;
		field->module = module;
		field->text = module->url;
		field->placeholder = "http://host:port/path";
		menu->addChild(field);
	}
};

Model* modelRadio = createModel<Radio, RadioWidget>("Radio");
