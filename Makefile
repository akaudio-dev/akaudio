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

# Resolve the target arch NOW (ARCH_MAC/WIN/LIN) so the FAAD2 sources below can be
# picked per-platform before plugin.mk finalizes OBJECTS — compile.mk does
# `OBJECTS := ... $(SOURCES)` (immediate) at include time, so SOURCES must be
# complete first. arch.mk only runs `$(CC) -dumpmachine` and sets ARCH_* vars;
# plugin.mk includes it again later, which is harmless (idempotent).
include $(RACK_DIR)/arch.mk

# Vendored FAAD2 — the HE-AAC (AAC-LC + SBR + PS) DECODER for AAC/HLS streams on
# Windows + Linux. macOS decodes AAC through the system AudioToolbox instead (see
# the ARCH_MAC block after `include plugin.mk`), so it doesn't compile FAAD2 at
# all. FAAD2's defaults are a float build with SBR+PS enabled; the only tweak is
# src/dep/faad2/config.h, applied via -DHAVE_CONFIG_H scoped to the FAAD2 objects
# below (NOT globally — libogg/libvorbis also test HAVE_CONFIG_H).
ifndef ARCH_MAC
FAAD2_SOURCES := $(wildcard src/dep/faad2/libfaad/*.c)
FLAGS += -I src/dep/faad2/include -I src/dep/faad2/libfaad
SOURCES += $(FAAD2_SOURCES)
endif

# Add files to the ZIP package when running `make dist`
DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += presets

# Include the Rack plugin Makefile framework
include $(RACK_DIR)/plugin.mk

# macOS-only: on mac the AAC decoder (src/net/AacDecoder.cpp) uses the system
# AudioToolbox instead of vendored FAAD2 (see the FAAD2 block above, gated
# `ifndef ARCH_MAC`), so link its frameworks here. ARCH_MAC is defined by arch.mk
# (pulled in via plugin.mk above), so this block must come after the include.
# Appended LDFLAGS still reach the link recipe (make expands recipe variables at
# build time).
ifdef ARCH_MAC
	LDFLAGS += -framework AudioToolbox -framework CoreFoundation
endif

# Scope FAAD2's config to just its own translation units: HAVE_CONFIG_H makes
# libfaad/common.h pull in src/dep/faad2/config.h (modern-libc code paths). A
# target-specific append keeps it off every other .c — notably libogg/libvorbis,
# which test HAVE_CONFIG_H and would try to include their own missing config.h.
#
# -fvisibility=hidden on the SAME objects is not an optimization — it is required
# for correctness on Linux (cost a hard crash in 2.0.3; see below). FAAD2's cfft.c
# defines cffti/cfftf/cfftb, and libRack.so ALSO exports those exact names from its
# own (FFTPACK) FFT with incompatible signatures:
#     FAAD2:    cfft_info *cffti(uint16_t n)          <- takes an integer
#     libRack:  void       cffti(int *n, float *wsave) <- takes pointers
# ELF puts libRack in the global lookup scope before plugin.so is dlopen'd, so
# FAAD2's own intra-library call (mdct.c: `mdct->cfft = cffti(N/4)`) got bound to
# *libRack's* cffti, which dereferenced the integer 64 as a pointer -> SIGSEGV the
# instant a stream hit NeAACDecInit. Hidden visibility makes the definitions local
# to plugin.so, so those calls bind at static-link time and cannot be interposed;
# it also stops us exporting these generic names to other plugins. AacDecoder.cpp
# is compiled without the flag and still links against the hidden NeAACDec* defs
# normally (hidden != unlinkable, just not dynamically exported) -- and Rack's
# init() stays exported because the flag is scoped to FAAD2's objects only.
# Do not drop this on a FAAD2 upgrade; verify with:
#     nm -D --defined-only plugin.so | grep -w cffti   # must print nothing
ifndef ARCH_MAC
$(patsubst %,build/%.o,$(FAAD2_SOURCES)): CFLAGS += -DHAVE_CONFIG_H -fvisibility=hidden
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

# Vendored OGG-Vorbis encoder sources (C) — the session test needs them for the OGG
# writes. Compiled here with $(CC) into build/ut_ogg (libvorbis uses C-only void* casts,
# so it can't go through $(CXX)); no config.h, matching the plugin build (§ Makefile note).
UT_OGG_INC := -I src/dep/libogg/include -I src/dep/libvorbis/include -I src/dep/libvorbis/lib
UT_OGG_SRCS := src/dep/libogg/src/bitwise.c src/dep/libogg/src/framing.c \
  $(filter-out %barkmel.c %psytune.c %tone.c %vorbisfile.c,$(wildcard src/dep/libvorbis/lib/*.c))
UT_OGG_OBJ := $(patsubst %.c,build/ut_ogg/%.o,$(UT_OGG_SRCS))
build/ut_ogg/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -O1 $(UT_OGG_INC) -c $< -o $@

# Rack-free unit tests (no Rack link, no OpenSSL): the interval clock, the looper engine,
# the looper session (disk), and the wire archive. `make unittest` builds + runs all.
.PHONY: unittest
unittest: $(UT_OGG_OBJ)
	@mkdir -p build
	$(CXX) -std=c++11 -O1 -I src test/jamclock_test.cpp -o build/jamclock_test && build/jamclock_test
	$(CXX) -std=c++11 -O1 -I src test/looper_engine_test.cpp src/looper/LooperEngine.cpp \
	  src/looper/LooperWorker.cpp -lpthread -o build/looper_engine_test && build/looper_engine_test
	rm -rf build/session_test_out
	$(CXX) -std=c++11 -O1 -I src $(UT_OGG_INC) test/session_test.cpp src/looper/Session.cpp \
	  src/net/ninjam/NjEncoder.cpp src/dep/stb_vorbis_impl.cpp $(UT_OGG_OBJ) -lpthread \
	  -o build/session_test && build/session_test build/session_test_out
	rm -rf build/archive_test_out
	$(CXX) -std=c++11 -O1 -I src test/archive_test.cpp src/net/ninjam/NjArchive.cpp \
	  src/net/Log.cpp -lpthread -o build/archive_test && build/archive_test build/archive_test_out

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
	  build/src/net/Socket.cpp.o build/src/net/Log.cpp.o \
	  build/src/net/Hls.cpp.o build/src/net/AacDecoder.cpp.o build/src/dep/dr_mp3_impl.cpp.o \
	  "$(RACK_ABS)/libRack.dylib" -undefined dynamic_lookup \
	  -framework AudioToolbox -framework CoreFoundation -o "$(LEAKDIR)/leak_stress"
	@# `leaks` strips DYLD_LIBRARY_PATH, so bake libRack's absolute path into the binary.
	install_name_tool -change libRack.dylib "$(RACK_ABS)/libRack.dylib" "$(LEAKDIR)/leak_stress"
	@echo "== build njclient_test (live NINJAM protocol) =="
	$(CXX) -std=c++11 -g $(LEAK_INC) test/njclient_test.cpp \
	  build/src/net/ninjam/NjClient.cpp.o build/src/net/ninjam/NjProtocol.cpp.o \
	  build/src/net/ninjam/NjAudio.cpp.o build/src/net/ninjam/NjEncoder.cpp.o \
	  build/src/net/Socket.cpp.o build/src/net/Log.cpp.o \
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
