#include "plugin.hpp"

Plugin* pluginInstance;

// Rack calls init() once when loading the shared library.
// Register every module's Model here.
void init(Plugin* p) {
	pluginInstance = p;

	p->addModel(modelNinjam);
	p->addModel(modelRadio);
}
