#include "plugin.hpp"
#include "net/Stream.hpp"

// NINJAM online jamming client. Skeleton only: outputs silence until the
// streaming/decoding layer (see net/Stream.hpp) is wired in.
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

	Ninjam() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configOutput(LEFT_OUTPUT, "Left");
		configOutput(RIGHT_OUTPUT, "Right");
	}

	void process(const ProcessArgs& args) override {
		// TODO: pull mixed remote audio from the NINJAM client (net/Stream.hpp)
		// and write it to the L/R outputs. Silent for now.
		outputs[LEFT_OUTPUT].setVoltage(0.f);
		outputs[RIGHT_OUTPUT].setVoltage(0.f);
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

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(13.0, 100.0)), module, Ninjam::LEFT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(27.0, 100.0)), module, Ninjam::RIGHT_OUTPUT));
	}
};

Model* modelNinjam = createModel<Ninjam, NinjamWidget>("Ninjam");
