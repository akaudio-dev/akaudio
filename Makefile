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

# Clean install: plugin.mk's `install` only copies the new .vcvplugin over the top of any
# existing install — it never removes the already-extracted "$(SLUG)/" folder, so presets
# (or any distributable) that were renamed/removed in a new layout linger in the user's
# Rack folder. Add a prerequisite that wipes the prior install — both the extracted folder
# and any old package — so every `make install` is a clean slate. This is an extra
# prerequisite line (no recipe), so it augments plugin.mk's install rule rather than
# overriding it. It depends on `dist` so the wipe only happens once the new package has
# built successfully (a failed build can't leave you with nothing installed).
install: clean-prev-install

clean-prev-install: dist
	rm -rf "$(PLUGINS_DIR)/$(SLUG)" "$(PLUGINS_DIR)/$(SLUG)"-*.vcvplugin

# Leak / memory-safety check (macOS). Links the standalone test harnesses against the
# already-built object files and runs them under Apple's `leaks` — the right tool here
# because Apple clang ships no LeakSanitizer on arm64. `leaks` exits non-zero when it
# finds leaks, so this target fails CI on a regression. Coverage:
#   * enc_test     — OFFLINE NINJAM OGG-Vorbis encode->decode (libvorbis/ogg/stb_vorbis).
#                    Deterministic, always run.
#   * leak_stress  — StreamClient lifecycle churn over MP3 + AAC/HLS (TLS/HTTP/decoder).
#                    Streams live public URLs, so it needs internet.
#   * njclient_test— live NINJAM protocol + interval decode. OPT-IN only: pass
#                    `NJ_HOST=<server>` (we never connect to a public server unattended).
# For memory safety (use-after-free), build a harness with `-fsanitize=address` and run
# it directly; on Linux use valgrind instead of `leaks`.
LEAKDIR := build/leakcheck
RACK_ABS := $(realpath $(RACK_DIR))
OGGVORBIS_OBJ := $(filter build/src/dep/libogg/% build/src/dep/libvorbis/%,$(OBJECTS))
LEAK_INC := -I src -I src/dep/libogg/include -I src/dep/libvorbis/include -I src/dep/libvorbis/lib

.PHONY: leakcheck
leakcheck: all
	@mkdir -p "$(LEAKDIR)"
	@echo "== build enc_test (offline NINJAM encode/decode) =="
	$(CXX) -std=c++11 -g $(LEAK_INC) test/enc_test.cpp \
	  build/src/net/ninjam/NjEncoder.cpp.o build/src/dep/stb_vorbis_impl.cpp.o $(OGGVORBIS_OBJ) \
	  -o "$(LEAKDIR)/enc_test"
	@echo "== build leak_stress (StreamClient: TLS/HTTP/HLS/AAC/MP3) =="
	$(CXX) -std=c++11 -g -I src -I $(RACK_DIR)/dep/include test/leak_stress.cpp \
	  build/src/net/Stream.cpp.o build/src/net/Http.cpp.o build/src/net/Tls.cpp.o \
	  build/src/net/Socket.cpp.o \
	  build/src/net/Hls.cpp.o build/src/net/AacDecoder.cpp.o build/src/dep/dr_mp3_impl.cpp.o \
	  "$(RACK_ABS)/libRack.dylib" -undefined dynamic_lookup \
	  -framework AudioToolbox -framework CoreFoundation -o "$(LEAKDIR)/leak_stress"
	@# `leaks` strips DYLD_LIBRARY_PATH, so bake libRack's absolute path into the binary.
	install_name_tool -change libRack.dylib "$(RACK_ABS)/libRack.dylib" "$(LEAKDIR)/leak_stress"
	@echo "== build njclient_test (live NINJAM protocol) =="
	$(CXX) -std=c++11 -g $(LEAK_INC) test/njclient_test.cpp \
	  build/src/net/ninjam/NjClient.cpp.o build/src/net/ninjam/NjProtocol.cpp.o \
	  build/src/net/ninjam/NjAudio.cpp.o build/src/net/ninjam/NjEncoder.cpp.o \
	  build/src/net/Socket.cpp.o \
	  build/src/dep/stb_vorbis_impl.cpp.o $(OGGVORBIS_OBJ) \
	  "$(RACK_ABS)/libRack.dylib" -undefined dynamic_lookup -o "$(LEAKDIR)/njclient_test"
	install_name_tool -change libRack.dylib "$(RACK_ABS)/libRack.dylib" "$(LEAKDIR)/njclient_test"
	@echo "\n== leaks: enc_test (offline, deterministic) =="
	@set -o pipefail; leaks --atExit -- "$(LEAKDIR)/enc_test" | grep -E 'leaks for|nodes malloced'
	@echo "\n== leaks: leak_stress x3 cycles (needs internet) =="
	@set -o pipefail; leaks --atExit -- "$(LEAKDIR)/leak_stress" 3 | grep -E 'leaks for|nodes malloced'
ifdef NJ_HOST
	@echo "\n== leaks: njclient_test ($(NJ_HOST)) =="
	@set -o pipefail; leaks --atExit -- "$(LEAKDIR)/njclient_test" "$(NJ_HOST)" 2049 8 akaudio-leakcheck | grep -E 'leaks for|nodes malloced'
else
	@echo "\n(skipping njclient_test — set NJ_HOST=<server> to leak-check the live NINJAM path)"
endif
	@echo "\nleakcheck: all harnesses reported 0 leaks (leaks(1) exits non-zero otherwise)"

.PHONY: clean-prev-install
