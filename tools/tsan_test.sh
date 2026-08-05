#!/bin/bash
# ThreadSanitizer builds of the Rack-free test harnesses (play_test, njclient_test).
# This is the check the static tools can't do: it validates the acquire/release
# ordering of the SPSC ring buffers and every bg-thread -> consumer handoff at
# runtime, on the real network paths.
#
# Usage:
#   tools/tsan_test.sh                    # build both into build/tsan/
#   build/tsan/play_test [url] [secs]     # then run under TSan
#   build/tsan/njclient_test <host> [port] [secs] [user] [pass] [tx|voicetx]
#
# Needs a Rack *source* tree (static libssl/libcrypto in dep/lib); the SDK-only
# fallback (link libRack.dylib + install_name_tool, per CLAUDE.md) is not wired
# up here. macOS-only as written (AudioToolbox frameworks).
set -e
cd "$(dirname "$0")/.." >/dev/null

RACK_DIR=${RACK_DIR:-../Rack}
SSL="$RACK_DIR/dep/lib/libssl.a $RACK_DIR/dep/lib/libcrypto.a"
[ -f "$RACK_DIR/dep/lib/libssl.a" ] || { echo "need a Rack source tree at $RACK_DIR (static OpenSSL)" >&2; exit 1; }

# -O1 keeps stacks honest without optimizing away the interleavings TSan watches.
TSAN="-fsanitize=thread -g -O1 -fno-omit-frame-pointer"
OUT=build/tsan
mkdir -p "$OUT/dep"

# Vendored libogg/libvorbis, TSan-instrumented (the encoder runs on the tx thread).
# Cached: only recompiled when missing.
OGGV_INC="-I src/dep/libogg/include -I src/dep/libvorbis/include -I src/dep/libvorbis/lib"
for c in src/dep/libogg/src/bitwise.c src/dep/libogg/src/framing.c src/dep/libvorbis/lib/*.c; do
	case "$(basename "$c")" in
		barkmel.c|psytune.c|tone.c|vorbisfile.c) continue ;; # standalone tools/decoder, excluded by the Makefile too
	esac
	o="$OUT/dep/$(basename "$c" .c).o"
	[ -f "$o" ] || cc $TSAN $OGGV_INC -c "$c" -o "$o" 2>/dev/null
done

echo "building $OUT/play_test..."
c++ -std=c++11 $TSAN -I src -I "$RACK_DIR/dep/include" \
	test/play_test.cpp \
	src/net/Stream.cpp src/net/Http.cpp src/net/Tls.cpp src/net/Hls.cpp \
	src/net/AacDecoder.cpp src/net/Socket.cpp src/net/Log.cpp \
	src/dep/dr_mp3_impl.cpp \
	$SSL \
	-framework AudioToolbox -framework CoreFoundation \
	-o "$OUT/play_test"

echo "building $OUT/njclient_test..."
c++ -std=c++11 $TSAN -I src -I "$RACK_DIR/dep/include" $OGGV_INC \
	test/njclient_test.cpp \
	src/net/ninjam/NjClient.cpp src/net/ninjam/NjProtocol.cpp \
	src/net/ninjam/NjAudio.cpp src/net/ninjam/NjEncoder.cpp \
	src/net/Socket.cpp src/net/Log.cpp \
	src/dep/stb_vorbis_impl.cpp \
	"$OUT"/dep/*.o \
	"$RACK_DIR/dep/lib/libcrypto.a" \
	-o "$OUT/njclient_test"

echo "done. run with e.g.:"
echo "  $OUT/play_test '' 10"
echo "  $OUT/njclient_test <host> 2049 20"
