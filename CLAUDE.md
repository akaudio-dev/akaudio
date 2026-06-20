# CLAUDE.md — Akozlov VCV Rack plugin

This file provides guidance to Claude Code (claude.ai/code) when working in this repository.

## What this is

A single VCV Rack **plugin** (slug `akozlov`) containing multiple **modules** — the Bidoo model: one slug, one shared library, one VCV Library page, many modules. See `../CLAUDE.md` for the surrounding workspace (the source-built Rack SDK at `../Rack`, and the rule to never touch the user's Rack Pro app in `/Applications`).

Modules:
- **Radio** (`src/Radio.cpp`) — streaming internet radio (Icecast/HTTP MP3) source. Implemented (v1).
- **Ninjam** (`src/Ninjam.cpp`) — NINJAM jam **listener**. Listening does NOT use the NINJAM protocol and does not join the server: public NINJAM communities publish a live Icecast/HTTP stream of each room's mix, so this reuses `StreamClient` (same as Radio) pointed at a room's stream URL. Joining as a (silent) protocol participant — connect/auth-anonymous/subscribe/decode-OGG-intervals, as in `~/work/jamauv3` — remains a separate, much larger future feature.

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
- `Http.hpp` / `Http.cpp` — `httpGet(url, out)`: a tiny blocking HTTP/1.0 GET for small text bodies (the NINJAM room directory JSON), http:// only. Must be called off the UI/audio thread (it blocks). Not for audio — streams go through `StreamClient`.
- `RoomDirectory.hpp` / `RoomDirectory.cpp` — background directory of public NINJAM rooms. `refresh()` fetches+parses `http://ninbot.com/app/servers.php` (jansson) on a worker thread into a mutex-guarded `vector<Room>`; the UI reads `rooms()` (sorted snapshot) and `status()` instantly, never blocking on the network. `Room.playUrl()` is the http MP3 mount `StreamClient` plays (ssl_stream is ignored in v1: no TLS).
- `dep/dr_mp3.h` — vendored single-header MP3 decoder (public domain / MIT-0). `dep/dr_mp3_impl.cpp` is the one TU that defines `DR_MP3_IMPLEMENTATION`.

v1 limitations (intentional, future work): **http:// only** (no TLS), **MP3 only** (no AAC/OGG), blocking connect (cancelled only via socket shutdown / OS timeout), linear (not band-limited) resampling.

## Adding a module

1. `src/<Name>.cpp`: a `Module` subclass + a `ModuleWidget` subclass + `Model* model<Name> = createModel<...>("<Name>");`.
2. `extern Model* model<Name>;` in `src/plugin.hpp`.
3. `p->addModel(model<Name>);` in `src/plugin.cpp`.
4. `res/<Name>.svg` panel (mm units; 1 HP = 5.08 mm wide, 128.5 mm tall) and a `modules[]` entry in `plugin.json`.

## Persistence

Module state is saved per-instance via `dataToJson`/`dataFromJson` (Radio persists `url` and `playing`, and auto-resumes playback on patch load). The slug strings in `plugin.json` and `createModel(...)` are the permanent identity — don't rename them once patches reference them.
