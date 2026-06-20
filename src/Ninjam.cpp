#include "plugin.hpp"
#include "net/Stream.hpp"

// NINJAM jam listener. Listening to a NINJAM jam does NOT use the NINJAM protocol
// and does NOT join the server — public NINJAM communities (ninjamer.com, ninbot,
// …) publish a live Icecast/HTTP stream of each room's mix, so we simply consume
// that stream like internet radio (reusing net/Stream.hpp, same as the Radio
// module). Joining as a (silent) protocol participant would be a separate, much
// larger feature; this is pure, external listening.
struct Ninjam : Module {
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
		CONNECTED_LIGHT,
		LIGHTS_LEN
	};

	akozlov::StreamClient stream;
	// The jam's Icecast/HTTP listen-stream URL (MP3). Paste a room's stream URL
	// from a public NINJAM community; empty by default since rooms come and go.
	std::string url = "";
	bool listening = false;

	Ninjam() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configOutput(LEFT_OUTPUT, "Left");
		configOutput(RIGHT_OUTPUT, "Right");
		stream.setSampleRate(APP->engine->getSampleRate());
	}

	void listen() {
		if (url.empty())
			return;
		stream.start(url);
		listening = true;
	}
	void stopListening() {
		stream.stop();
		listening = false;
	}
	void toggleListen() {
		if (listening)
			stopListening();
		else
			listen();
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
		lights[CONNECTED_LIGHT].setBrightnessSmooth(live ? 1.f : 0.f, args.sampleTime);
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "url", json_string(url.c_str()));
		json_object_set_new(root, "listening", json_boolean(listening));
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "url"))
			url = json_string_value(j);
		if (json_t* j = json_object_get(root, "listening"))
			listening = json_boolean_value(j);
		if (listening)
			listen();
	}
};

// Editable jam-stream URL field shown in the context menu.
struct NinjamUrlField : ui::TextField {
	Ninjam* module = nullptr;
	void onAction(const ActionEvent& e) override {
		if (module)
			module->url = text;
		ui::TextField::onAction(e);
	}
};

struct NinjamWidget : ModuleWidget {
	NinjamWidget(Ninjam* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Ninjam.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(20.32, 30.0)), module, Ninjam::CONNECTED_LIGHT));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(13.0, 100.0)), module, Ninjam::LEFT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(27.0, 100.0)), module, Ninjam::RIGHT_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Ninjam* module = getModule<Ninjam>();
		if (!module)
			return;

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Status: " + module->stream.getStatusText()));

		menu->addChild(createMenuItem(module->listening ? "Stop" : "Listen", "", [module]() {
			module->toggleListen();
		}));

		menu->addChild(createMenuLabel("Jam stream URL (http:// Icecast MP3):"));
		NinjamUrlField* field = new NinjamUrlField;
		field->box.size.x = 260;
		field->module = module;
		field->text = module->url;
		field->placeholder = "http://host:port/room";
		menu->addChild(field);
	}
};

Model* modelNinjam = createModel<Ninjam, NinjamWidget>("Ninjam");
