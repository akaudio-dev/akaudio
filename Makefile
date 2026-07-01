# Where to find Rack's plugin build framework + import library. Two supported sources:
#   1. A sibling Rack *source* checkout (../Rack) — the original peer-of-enlistment setup.
#   2. The official downloadable Rack *SDK* (../Rack-SDK) — no source checkout needed;
#      run `tools/get_sdk.sh` to fetch it. The SDK alone is enough to build+link.
# Prefer the source build if present, else fall back to the SDK. Override with `make RACK_DIR=...`.
RACK_DIR ?= $(firstword $(wildcard ../Rack ../Rack-SDK) ../Rack)

# FLAGS will be passed to both the C and C++ compiler
FLAGS +=
CFLAGS +=
CXXFLAGS +=

# Careful about linking to shared libraries, since you can't assume much about the user's environment and library search path.
LDFLAGS +=

# Add all source files (recursively, so src/net/*.cpp etc. are included) to the build
SOURCES += $(shell find src -name '*.cpp')

# Vendored libogg + libvorbis (for OGG Vorbis ENCODING — transmit). Compiled directly
# from source (self-contained; no separate `make dep`). stb_vorbis still handles decode.
# Exclude the standalone tools (barkmel/psytune/tone, which have their own main()) and the
# unused vorbisfile decode-convenience layer.
FLAGS += -I src/dep/libogg/include -I src/dep/libvorbis/include -I src/dep/libvorbis/lib
SOURCES += src/dep/libogg/src/bitwise.c src/dep/libogg/src/framing.c
SOURCES += $(filter-out %barkmel.c %psytune.c %tone.c %vorbisfile.c, $(wildcard src/dep/libvorbis/lib/*.c))

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

# Windows-only: the net/ layer's sockets are Winsock2 (src/net/Socket.hpp maps the
# POSIX BSD-socket API onto it). Link the Winsock 2 import library. OpenSSL/SHA1
# symbols (TLS) resolve from libRack's exports like on the other platforms.
ifdef ARCH_WIN
	LDFLAGS += -lws2_32
endif
