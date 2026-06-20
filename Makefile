# If RACK_DIR is not defined when calling the Makefile, default to two directories above (the sibling Rack source build).
RACK_DIR ?= ../Rack

# FLAGS will be passed to both the C and C++ compiler
FLAGS +=
CFLAGS +=
CXXFLAGS +=

# Careful about linking to shared libraries, since you can't assume much about the user's environment and library search path.
LDFLAGS +=

# Add all source files (recursively, so src/net/*.cpp etc. are included) to the build
SOURCES += $(shell find src -name '*.cpp')

# Add files to the ZIP package when running `make dist`
DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += presets

# Include the Rack plugin Makefile framework
include $(RACK_DIR)/plugin.mk

# macOS-only: the AAC decoder (src/net/AacDecoder.cpp) uses the system
# AudioToolbox. ARCH_MAC is defined by arch.mk (pulled in via plugin.mk above),
# so this block must come after the include. Appended LDFLAGS still reach the
# link recipe (make expands recipe variables at build time).
ifdef ARCH_MAC
	LDFLAGS += -framework AudioToolbox -framework CoreFoundation
endif
