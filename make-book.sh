#!/usr/bin/env bash
# Weave this plugin's source into a book with trusty_weaver.
#   ./make-book.sh            -> build/akozlov.epub
# Override the weaver location with TRUSTY_WEAVER=/path/to/trusty_weaver.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TW="${TRUSTY_WEAVER:-$HOME/work/trusty_weaver}"

mkdir -p "$HERE/build"
# Build the weaver once, then run it from here so chapter names stay relative.
go build -C "$TW" -o "$HERE/build/trusty_weaver" .

cd "$HERE"
# Files in deliberate reading order: identity -> realtime bridge -> streaming
# core -> transport/codec layers -> room directory -> the two modules.
build/trusty_weaver \
  -title "Akozlov" \
  -subtitle "A VCV Rack plugin" \
  -author "Andrei Kozlov" \
  -preface README.md \
  -o build/akozlov.epub \
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
  src/net/RoomDirectory.hpp \
  src/net/RoomDirectory.cpp \
  src/Radio.cpp \
  src/Ninjam.cpp
