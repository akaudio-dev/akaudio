#!/usr/bin/env bash
# Weave this plugin's source into a book with trusty_weaver.
#   ./make-book.sh            -> build/akaudio.epub
# Override the weaver location with TRUSTY_WEAVER=/path/to/trusty_weaver.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TW="${TRUSTY_WEAVER:-$HOME/work/trusty_weaver}"

mkdir -p "$HERE/build"
# Build the weaver once, then run it from here so chapter names stay relative.
go build -C "$TW" -o "$HERE/build/trusty_weaver" .

cd "$HERE"
# Files in deliberate reading order: identity -> realtime bridge -> streaming
# core -> transport/codec layers -> Radio's image cache + add-from-URL import ->
# room directory -> NINJAM protocol stack -> the two modules.
build/trusty_weaver \
  -title "AK Audio" \
  -subtitle "A VCV Rack plugin" \
  -author "Andrei Kozlov" \
  -preface README.md \
  -o build/akaudio.epub \
  src/plugin.hpp \
  src/plugin.cpp \
  src/net/RingBuffer.hpp \
  src/net/Stream.hpp \
  src/net/Stream.cpp \
  src/net/Tls.hpp \
  src/net/Tls.cpp \
  src/net/Http.hpp \
  src/net/Http.cpp \
  src/net/AacDecoder.hpp \
  src/net/AacDecoder.cpp \
  src/net/Hls.hpp \
  src/net/Hls.cpp \
  src/net/ImageCache.hpp \
  src/net/ImageCache.cpp \
  src/net/StationImport.hpp \
  src/net/StationImport.cpp \
  src/net/RoomDirectory.hpp \
  src/net/RoomDirectory.cpp \
  src/net/ninjam/NjProtocol.hpp \
  src/net/ninjam/NjProtocol.cpp \
  src/net/ninjam/NjAudio.hpp \
  src/net/ninjam/NjAudio.cpp \
  src/net/ninjam/NjEncoder.hpp \
  src/net/ninjam/NjEncoder.cpp \
  src/net/ninjam/NjClient.hpp \
  src/net/ninjam/NjClient.cpp \
  src/ClickableLed.hpp \
  src/Radio.cpp \
  src/Ninjam.cpp
