# CLAUDE.md — Akozlov VCV Rack plugin

This file provides guidance to Claude Code (claude.ai/code) when working in this repository.

## What this is

A single VCV Rack **plugin** (slug `akozlov`) containing multiple **modules** — the Bidoo model: one slug, one shared library, one VCV Library page, many modules. See `../CLAUDE.md` for the surrounding workspace (the source-built Rack SDK at `../Rack`, and the rule to never touch the user's Rack Pro app in `/Applications`).

Modules:
- **Radio** (`src/Radio.cpp`) — streaming internet radio (Icecast/HTTP MP3) source. Implemented (v1).
- **Ninjam** (`src/Ninjam.cpp`) — NINJAM jam **listener**. Listening does NOT use the NINJAM protocol and does not join the server: public NINJAM communities publish a live Icecast/HTTP stream of each room's mix, so this reuses `StreamClient` (same as Radio) pointed at a room's stream URL. Joining as a (silent) protocol participant — connect/auth-anonymous/subscribe/decode-OGG-intervals, as in `~/work/jamauv3` — remains a separate, much larger future feature. The panel is **20 HP** and hosts an in-panel browser of public rooms (search, scrollable list, per-row click-to-listen, peak meter) fed by `net/RoomDirectory` (background fetch of ninbot's directory); the room data and list never block the UI thread. See `TODO.md` ("Ninjam: server picker") for the design.

Both are "network audio → Rack engine" and share `src/net/`.

## Build / install

```bash
export RACK_DIR=/Users/akozlov/work/VCV/Rack   # or rely on Makefile default ../Rack
make                 # -> plugin.dylib
make install         # package + install into ~/Library/Application Support/Rack2/plugins-mac-arm64/
make clean
```

`make install` writes into the **shared** Rack user folder, i.e. the same plugins folder the user's Rack Pro app reads (the user has accepted this). The `Makefile` globs all of `src/**/*.cpp` via `find`, so new files under `src/` are picked up automatically.

## THE REALTIME / THREADING CONTRACT (most important thing to get right)

Rack runs `Module::process(args)` on the **audio thread**, once per sample frame (tens of thousands of times per second). In `process()` and anything it calls:

- **Never block, allocate, take a lock, or do I/O.** No `new`/`malloc`, no `std::mutex`, no syscalls, no network, no file access, no logging. Doing so causes audio dropouts/xruns.
- All networking and decoding happens on a **background thread** (`akozlov::StreamClient`, `src/net/Stream.cpp`).
- The two threads communicate **only** through a lock-free single-producer/single-consumer ring buffer (`akozlov::StereoRingBuffer`, `src/net/RingBuffer.hpp`): the net thread `push()`es decoded frames, `process()` `pull()`s one frame. `pull()` returns false on underrun → output silence, never wait.

Control flow rules:
- **Start/stop the stream from the UI/main thread only** (context-menu actions, `dataFromJson`), never from `process()`. `StreamClient::stop()` joins the background thread — joining on the audio thread would stall audio.
- `pull()` is the only `StreamClient` method safe to call from `process()`.
- Update the engine sample rate via `onSampleRateChange()` → `StreamClient::setSampleRate()` (atomic; the resampler reads it per block).

## net/ layer design (`src/net/`)

- `RingBuffer.hpp` — lock-free SPSC stereo ring buffer; power-of-two capacity; atomic head/tail with acquire/release ordering.
- `Stream.hpp` / `Stream.cpp` — `StreamClient`. Background thread: parse `http://host:port/path` → blocking socket connect → HTTP/1.0 GET with `Icy-MetaData: 0` (keeps the MP3 body clean) → stream body through **dr_mp3** (pull model; our `onRead` serves post-header leftover bytes then `recv()`s) → **linear resample** from stream rate to engine rate → `push()` to the ring. Backpressure: when the ring is full the producer briefly sleeps and retries, so it reads from the socket at playback speed and never drops audio. `stop()` sets an abort flag and `shutdown()`s the socket to interrupt a blocked `recv()`; `run()` owns the `close()`.
  - **Non-seekable init (important):** `drmp3_init` is called with **NULL** seek/tell callbacks. dr_mp3 probes the first 10 bytes for an ID3v2 tag; on a live stream those are MP3 audio, so it tries to rewind — and a seek callback that returns `DRMP3_FALSE` makes init **abort** (`"Not an MP3 stream"`). Passing NULL takes dr_mp3's no-seek path: it discards those 10 bytes and the decoder re-syncs to the next frame. Without this, *no* live Icecast stream plays.
- `../test/play_test.cpp` — standalone smoke test for the audio path (links only the net layer + dr_mp3, no Rack): drives `StreamClient` against a live stream and reports decoded-audio stats. Build/run: `c++ -std=c++11 -I src test/play_test.cpp src/net/Stream.cpp src/dep/dr_mp3_impl.cpp -o build/play_test && build/play_test [url] [seconds]`.
- `Http.hpp` / `Http.cpp` — `httpGet(url, out)`: a tiny blocking HTTP/1.0 GET for small text bodies (the NINJAM room directory JSON), http:// only. Must be called off the UI/audio thread (it blocks). Not for audio — streams go through `StreamClient`.
- `RoomDirectory.hpp` / `RoomDirectory.cpp` — background directory of public NINJAM rooms. `refresh()` fetches+parses `http://ninbot.com/app/servers.php` (jansson) on a worker thread into a mutex-guarded `vector<Room>`; the UI reads `rooms()` (sorted snapshot) and `status()` instantly, never blocking on the network. `Room.playUrl()` is the http MP3 mount `StreamClient` plays (ssl_stream is ignored in v1: no TLS).
- `dep/dr_mp3.h` — vendored single-header MP3 decoder (public domain / MIT-0). `dep/dr_mp3_impl.cpp` is the one TU that defines `DR_MP3_IMPLEMENTATION`.

- `Tls.hpp` / `Tls.cpp` — minimal TLS client over an already-connected fd, using the **OpenSSL that `libRack` exports** (535 SSL symbols; resolved at load via `-undefined dynamic_lookup`, no new dep). `tlsHandshake` does the handshake with **SNI**; `tlsRead`/`tlsWrite` fall back to plain `recv`/`send` when TLS is inactive, so the http and https paths share one code path. Certificate verification is **not enforced** (`SSL_VERIFY_NONE`, no bundled CA store) — adequate for public audio streams; tighten later if we ship a CA bundle. Both `StreamClient` and `httpGet` accept `https://`.

- `AacDecoder.hpp` / `AacDecoder.cpp` — streaming ADTS-AAC decoder, **macOS only** (uses the system `AudioToolbox`: `AudioFileStream` parses ADTS into packets, `AudioConverter` → Float32 stereo). `#if defined(__APPLE__)`-gated; on other platforms `available()`/`init()` return false and the caller reports "AAC needs macOS". Push model: `feed()` raw bytes, decoded PCM arrives via `onPCM`. The Makefile links `-framework AudioToolbox -framework CoreFoundation` under `ifdef ARCH_MAC` (after `include plugin.mk`, where `ARCH_MAC` is defined).
- **Codec is chosen at runtime** in `StreamClient::run` from the response `Content-Type` (`audio/aac`/`aacp`/`application/aac` → AAC; otherwise MP3 via dr_mp3). Both decoders feed the same linear resampler + backpressure push (`pushFrame` lambda).

Current limitations (future work): **no OGG/Vorbis** yet; AAC is macOS-only (other platforms fall back to an error); blocking connect (cancelled only via socket shutdown / OS timeout); linear (not band-limited) resampling.

## Adding a module

1. `src/<Name>.cpp`: a `Module` subclass + a `ModuleWidget` subclass + `Model* model<Name> = createModel<...>("<Name>");`.
2. `extern Model* model<Name>;` in `src/plugin.hpp`.
3. `p->addModel(model<Name>);` in `src/plugin.cpp`.
4. `res/<Name>.svg` panel (mm units; 1 HP = 5.08 mm wide, 128.5 mm tall) and a `modules[]` entry in `plugin.json`.

## Persistence

Module state is saved per-instance via `dataToJson`/`dataFromJson` (Radio persists `url`, `stationName`, `playing`; auto-resumes playback on patch load). The slug strings in `plugin.json` and `createModel(...)` are the permanent identity — don't rename them once patches reference them.

## Stations = factory presets (Radio)

Radio's "stations" are **curated factory presets**, not a bespoke database — we lean on Rack's native preset system. Each `.vcvm` is a module `toJson()` (`plugin`/`model`/`version`/`params`/`data`) whose `data` is `dataToJson` (`url`, `stationName`, `icon`, `playing:true`). Rack lists them under right-click → **Preset → (factory presets)**; we also surface them on-panel via a `StationChoice` (LedDisplayChoice) and a context-menu **Stations** submenu, both loading the chosen file through the public `ModuleWidget::loadAction` (so it's the real preset loader, with undo).

**Theme (deliberate): ambient/utility sound *sources*, NOT music.** Radio feeds a modular patch, so a fixed-tempo song is useless/distracting — the curated set is raw material for granular/ambient processing: **Nature** (Ambi Nature Radio, MyNoise Pure Nature, Nature Radio Rain), **Space** (Echoes of Blue Mars: Blue Mars / Cryosleep / Voices From Within), **Scanners** (Broadcastify police/fire/EMS feeds). All confirmed live `audio/mpeg` before shipping (liveness is mandatory — niche feeds rot; see `TODO.md`). Future: **Spoken** (BBC Radio 4 et al.) needs HLS; **ATC** (LiveATC) is `.pls`-wrapped + ToS-gray — both deferred.

**Grouping.** Presets live in **category subfolders** (`presets/Radio/<Category>/NN_<Name>.vcvm`). `appendStationDir()` recurses: a subfolder becomes a submenu, a `.vcvm` becomes a station row — mirroring Rack's own factory-preset folder convention, so the native Preset menu groups identically. **Add a station = drop a verified `.vcvm` in the right `presets/Radio/<Category>/`.** Picking one calls `loadAction` → `dataFromJson`, which stops any current stream and starts the new URL.

**Station artwork (visual ID).** Each station carries an `icon` = plugin-relative PNG path (e.g. `stations/groovesalad.png`) under `res/stations/`. We **bundle** the art (fetched from SomaFM's logo API at build time, 256² PNG — NanoVG decodes png/jpg/gif but **not .ico**, so favicons must be PNG). It renders via `drawStationArt()` (`APP->window->loadImage` + `nvgImagePattern`, cached by path): a large square on the panel (`StationArt`) and a right-aligned thumbnail per row in the picker (`StationItem`, with a green ring on the current station). A hand-typed URL clears `icon` (no art → ♪ placeholder). **Runtime favicon fetching is deferred** to the radio-browser.info phase (download bytes off-thread via `httpGet`, `nvgCreateImageMem` on the UI thread, disk cache; PNG-only).
