#include "plugin.hpp"
#include "net/Stream.hpp"
#include "net/RoomDirectory.hpp"

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
	// Background directory of public jam rooms (ninbot). Fetched off-thread; the
	// UI reads its cache instantly. Not persisted — rooms come and go.
	akozlov::RoomDirectory directory;
	// The jam's Icecast/HTTP listen-stream URL (MP3). Set by picking a room or
	// typing one; empty by default since rooms come and go.
	std::string url = "";
	// Human label for the picked room (the room name, or the URL host). Shown on
	// the panel and persisted alongside the URL.
	std::string roomLabel = "";
	bool listening = false;

	Ninjam() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configOutput(LEFT_OUTPUT, "Left");
		configOutput(RIGHT_OUTPUT, "Right");
		stream.setSampleRate(APP->engine->getSampleRate());
		directory.refresh();
	}

	// Point at a room's stream and start listening (an explicit pick = listen).
	void selectRoom(const akozlov::Room& room) {
		if (listening)
			stopListening();
		url = room.playUrl();
		roomLabel = room.name.empty() ? room.host : room.name;
		listen();
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
		json_object_set_new(root, "roomLabel", json_string(roomLabel.c_str()));
		json_object_set_new(root, "listening", json_boolean(listening));
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "url"))
			url = json_string_value(j);
		if (json_t* j = json_object_get(root, "roomLabel"))
			roomLabel = json_string_value(j);
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
		if (module) {
			module->url = text;
			module->roomLabel = "Custom";
		}
		ui::TextField::onAction(e);
	}
};

// Populate a menu with the cached room list. Kicks a background refresh first so
// the data freshens for next time, but always renders instantly from the cache —
// it never blocks the UI thread on the network.
static void appendRoomMenu(Menu* menu, Ninjam* module) {
	module->directory.refresh();

	menu->addChild(createMenuLabel(module->directory.status()));
	menu->addChild(createMenuItem("Refresh rooms", "", [module]() {
		module->directory.refresh();
	}));

	std::vector<akozlov::Room> rooms = module->directory.rooms();
	menu->addChild(new MenuSeparator);

	int shown = 0;
	for (const akozlov::Room& room : rooms) {
		if (!room.playable())
			continue; // http MP3 stream only (no TLS in v1)
		std::string cap = room.userMax > 0
			? string::f("%d/%d", room.userCount, room.userMax)
			: string::f("%d", room.userCount);
		std::string label = room.name + "   " + cap + " \xe2\x80\xa2 " + string::f("%d BPM", room.bpm);
		std::string playUrl = room.playUrl();
		bool current = (playUrl == module->url);
		menu->addChild(createCheckMenuItem(label, "",
			[current]() { return current; },
			[module, room]() { module->selectRoom(room); }));
		shown++;
	}
	if (shown == 0)
		menu->addChild(createMenuLabel("No playable rooms — try Refresh"));
}

// On-panel room picker: shows the current room, opens the cached room menu.
struct NinjamRoomChoice : LedDisplayChoice {
	Ninjam* module = nullptr;

	void step() override {
		if (module)
			text = module->roomLabel.empty() ? "Pick a room\xe2\x80\xa6" : module->roomLabel;
		else
			text = "Pick a room\xe2\x80\xa6";
		LedDisplayChoice::step();
	}

	void onAction(const ActionEvent& e) override {
		if (!module)
			return;
		ui::Menu* menu = createMenu();
		menu->addChild(createMenuLabel("Public NINJAM rooms"));
		appendRoomMenu(menu, module);
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

		// On-panel room picker label (click to open the cached room menu).
		NinjamRoomChoice* choice = new NinjamRoomChoice;
		choice->module = module;
		choice->box.pos = mm2px(Vec(2.5, 14.0));
		choice->box.size = mm2px(Vec(35.64, 8.0));
		addChild(choice);

		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(20.32, 32.0)), module, Ninjam::CONNECTED_LIGHT));

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

		menu->addChild(new MenuSeparator);
		menu->addChild(createSubmenuItem("Public rooms", module->roomLabel, [module](Menu* sub) {
			appendRoomMenu(sub, module);
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
