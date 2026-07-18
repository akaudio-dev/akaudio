# CLAUDE.md — AK Audio VCV Rack plugin

Guidance for Claude Code working in this repo. See `../CLAUDE.md` for the surrounding
workspace (the source-built Rack SDK at `../Rack`; never touch the user's Rack Pro app).

## What this is

A single VCV Rack **plugin** (slug `akaudio`) with multiple **modules**:
one slug, one shared library, one Library page, two modules (for now). Both modules are
"network audio → Rack engine" and share `src/net/`.

- **Radio** (`src/Radio.cpp`) — streaming internet radio source (Icecast/HTTP, MP3 /
  AAC / HLS). Ships some factory presets (see "Stations" below). Panel follows VCV's
  core/Fundamental house style and the standard **AUDIO** control vocabulary: `ebebeb→e1e1e1`
  gradient, `#1f1f1f` Nunito-Bold title, a **built-in VCA** on a `RoundLargeBlackKnob`
  **LEVEL** knob with the AUDIO taper (`configParam(0,2,1,"Level"," dB",-10,40)`, gain =
  `param²`, −∞…+12 dB) + its gauge ring, optional unipolar 0–10 V CV, and a dark
  `#1f1f1f` output plate with LEFT/RIGHT. A ▲/▼ stepper cycles all stations (deduped by
  URL) via the preset loader; the on-panel picker opens the grouped station menu.
  - **Add a station from a URL:** paste a stream URL in the context menu and it
    *auditions* — `net/StationImport` (off-thread) verifies real audio actually flows
    (`StreamClient::producedFrames()`, not just a connection), identifies it via
    radio-browser `byurl` (real name + favicon), fetches+caches the favicon
    (`net/ImageCache`, incl. `.ico`→PNG/BMP), then saves a user preset. Identified
    streams auto-save; unknown-but-playing ones prompt for a name; a failed audition
    rolls back to the previous station and shows the reason on the panel. No junk is ever
    saved, and saves dedup by URL against bundled + user presets.
- **Ninjam** (`src/Ninjam.cpp`) — NINJAM jam client, **two paths**:
  - **LISTEN** — zero-dependency: consume a room's public Icecast/HTTP mix via
    `StreamClient` (same as Radio). No protocol, no join.
  - **JOIN** — the real **NINJAM protocol** (`src/net/ninjam/`): connect, anonymous
    SHA1 auth, subscribe, decode the live multi-user OGG interval mix, **and transmit**
    (poly IN jacks → downbeat-aligned OGG-Vorbis encode → upload; heard by other
    clients). Room chat (send/recv) works. 20 HP panel with an in-panel room browser
    (search, scrollable list, click-to-listen/join, peak meter) fed by
    `net/RoomDirectory` (background fetch of ninbot's directory). UI never blocks on the
    network. Open polish items in `TODO.md`.

## Build / install

You do **not** need a Rack *source* checkout. The Makefile builds against either a
sibling Rack source tree (`../Rack`) **or** the official downloadable Rack **SDK**
(`../Rack-SDK`), auto-detecting whichever exists (source preferred). The SDK alone is
enough: it ships `plugin.mk`, `include/`, and `libRack.dylib` (the import library whose
OpenSSL exports our TLS/SHA1 code resolves against — `-undefined dynamic_lookup` defers
those to load time in the Rack app, so an SDK link is identical to a source link).

```bash
tools/get_sdk.sh     # one-time: download the SDK into ../Rack-SDK (host OS/arch auto-picked)
make                 # auto-detects ../Rack-SDK (or ../Rack) -> plugin.dylib
make install         # package + install into ~/Library/Application Support/Rack2/plugins-mac-arm64/
make clean
```

Override the framework location explicitly with `make RACK_DIR=/path/to/Rack-SDK`.
`tools/get_sdk.sh` defaults to Rack SDK 2.6.4 (`RACK_SDK_VERSION=...` to change) and
selects the mac-arm64 / mac-x64 / lin-x64 / win-x64 zip for the host.

`make install` writes the **shared** Rack user folder (also read by the user's Rack Pro
app — accepted). The Makefile globs `src/**/*.cpp` via `find`, so new files under `src/`
are picked up automatically. It also compiles vendored **libogg + libvorbis**
(`src/dep/libogg`, `src/dep/libvorbis`) directly — for OGG-Vorbis *encoding* (NINJAM
transmit); no separate `make dep`. Decoding uses **stb_vorbis** (NINJAM intervals) and
**dr_mp3** (radio). macOS-only AAC links `-framework AudioToolbox -framework CoreFoundation`
(guarded by `ifdef ARCH_MAC`, after `include plugin.mk`).

## THE REALTIME / THREADING CONTRACT (most important thing to get right)

Rack runs `Module::process(args)` on the **audio thread**, once per sample frame (tens of
thousands of times/sec). In `process()` and anything it calls:

- **Never block, allocate, lock, or do I/O.** No `new`/`malloc`, no `std::mutex`, no
  syscalls, network, file access, or logging — any of these causes audio dropouts/xruns.
- All networking + decoding + encoding runs on **background threads** (`StreamClient`,
  `nj::NjClient`).
- The threads communicate **only** through lock-free SPSC ring buffers
  (`akaudio::StereoRingBuffer`, `src/net/RingBuffer.hpp`): the bg thread `push()`es
  decoded frames, `process()` `pull()`s one. `pull()` returns false on underrun → output
  silence, never wait.

Control flow rules:
- **Start/stop streams/clients from the UI/main thread only** (menu actions,
  `dataFromJson`), never from `process()` — `stop()` joins the bg thread.
- `pull()` is the only such method safe to call from `process()`.
- Update sample rate via `onSampleRateChange()` → `setSampleRate()` (atomic; the
  resampler reads it per block).

## net/ layer (`src/net/`)

- `RingBuffer.hpp` — lock-free SPSC stereo ring; power-of-two capacity; atomic head/tail
  with acquire/release ordering.
- `Stream.{hpp,cpp}` — `StreamClient`. Bg thread: parse URL → **non-blocking** connect
  (polls `abort` every 100 ms so `stop()` can't be blocked by a slow/dead host) → HTTP/1.0
  GET (`Icy-MetaData: 0`) → decode → **linear resample** to engine rate → `push()`.
  Backpressure: when the ring is full the producer sleeps+retries, so it reads at
  playback speed and never drops audio. `stop()` aborts via `abort` + socket `shutdown()`.
  - `producedFrames()` counts frames actually decoded+pushed (resets on `start()`), so
    callers tell "audio is flowing" from merely "decoder inited / connected" — used for
    the honest LED and for verifying an auditioned URL.
  - Sockets set `SO_NOSIGPIPE`; the plugin also `signal(SIGPIPE, SIG_IGN)`s in `init()`
    (a write to a `shutdown()`-ed socket otherwise terminates the host, no crash report).
  - **Codec chosen at runtime** from `Content-Type`: AAC (`audio/aac`/`aacp`) → AAC path;
    else MP3 via dr_mp3. Both feed the same resampler + backpressure push.
  - **Non-seekable MP3 init (critical):** `drmp3_init` is called with **NULL** seek/tell
    callbacks. A seek callback returning false makes init abort ("Not an MP3 stream") on
    live streams (dr_mp3 probes the first 10 bytes for ID3v2 and tries to rewind). NULL
    takes the no-seek path. Without this, *no* live Icecast stream plays.
  - **Playlist resolution:** auto-resolves `.pls`/`.m3u` (not `.m3u8`) to the first stream
    URL before connecting.
- `Http.{hpp,cpp}` — `httpGet(url, out, abort=nullptr)`: HTTP(S) GET for small text/binary
  bodies (room/station JSON, playlists, favicons). Off-thread only; not for audio.
  **Bounded + interruptible** so a caller that `join()`s its thread (StreamClient on a
  station switch) never freezes: non-blocking connect, `SO_RCVTIMEO` + an idle ceiling
  (a silent peer can't hang `recv`), and the optional `abort` flag bails an in-flight
  request fast. Follows redirects (recovers http→https favicons).
- `Tls.{hpp,cpp}` — minimal TLS over a connected fd using the **OpenSSL `libRack`
  exports** (resolved via `-undefined dynamic_lookup`, no new dep). SNI handshake;
  `tlsRead`/`tlsWrite` fall back to plain `recv`/`send` when inactive so http/https share
  one path. `tlsRead` returns `-2` on would-block/timeout (vs `0` EOF / `-1` error) so
  `httpGet` can poll `abort` and retry. Cert verification **not enforced**
  (`SSL_VERIFY_NONE`) — fine for public audio; tighten if we ship a CA bundle.
- `Log.{hpp,cpp}` — `netLog(msg)`: pluggable diagnostics for the Rack-free net/ layer.
  **Logs only the abnormal** (failures with reasons+timings, unexpected stream endings,
  idle timeouts) — the healthy path is silent, so a quiet `log.txt` means a healthy
  plugin and every logged line deserves attention. plugin `init()` routes it into
  Rack's `log.txt` (`akaudio.net:` lines); play_test routes it to stderr. Failure
  triage = grep the log, no debugger. Never per-packet, never on the audio thread.
- `ImageCache.{hpp,cpp}` — `cacheImage(bytes, dir, key)`: normalize a downloaded favicon
  to a stb-loadable file on disk. PNG/JPG/GIF pass through; `.ico` is parsed (embedded
  PNG used as-is, or a DIB frame wrapped in a BMP header) since NanoVG/stb can't read the
  ICO container. SVG/unknown → "" (caller falls back to the synth avatar). Rack-free.
- `StationImport.{hpp,cpp}` — `StationImporter`: the off-thread "add this URL" worker for
  Radio. Verify (poll `StreamClient` state+`producedFrames`) → identify (radio-browser
  `byurl`) → fetch favicon (`ImageCache`). Does **not** write presets — returns a result
  the UI applies (commit+save / name-to-save / rollback). Rack-free (paths passed in).
- `AacDecoder.{hpp,cpp}` — streaming ADTS-AAC, **macOS only** (`AudioToolbox`). Push
  model: `feed()` bytes → PCM via `onPCM`. Off-macOS: `available()` false.
- `Hls.{hpp,cpp}` + `StreamClient::runHls` — **HLS** (`.m3u8`), **macOS only** (rides the
  AAC path). Resolves master→variant, polls the live media playlist, fetches each `.ts`
  segment, `tsExtractAdts` (tiny MPEG-TS→ADTS demux) → `AacDecoder`. `looksLikeHls`
  routes `run`→`runHls`. Makes BBC Radio 4 etc. playable.
- `RoomDirectory.{hpp,cpp}` — background directory of public NINJAM rooms. `refresh()`
  fetches+parses `http://ninbot.com/app/servers.php` (jansson) into a mutex-guarded
  `vector<Room>`; UI reads `rooms()`/`status()` instantly. `Room.playUrl()` is the http
  MP3 mount (ssl_stream ignored). **Privacy: the first fetch must be user-initiated** —
  the module ctor does NOT call `refresh()`, so adding the module or opening a patch never
  contacts ninbot. The room browser loads on an explicit action (Refresh button/menu, a
  click in the list, or focusing search) and only then does the 30 s auto-refresh keep the
  loaded list fresh (gated on `generation()>0 || loading()`).
- `ninjam/` — the NINJAM protocol stack:
  - `NjProtocol.{hpp,cpp}` — wire format (`[type u8][size u32 LE][payload]`, NUL-term
    UTF-8 strings, LE ints), message build/parse, SHA1 auth
    (`passhash=SHA1("user:pass")`, `response=SHA1(passhash+challenge8)`), chat
    (`parseChat`/`buildChat`).
  - `NjClient.{hpp,cpp}` — owns a TCP socket (port 2049) on a bg thread: connect, auth,
    keepalive, metadata (tempo/roster), interval reassembly, decode/mix, transmit, chat.
  - `NjAudio.{hpp,cpp}` — per-user interval decode (stb_vorbis) + tempo-driven interval
    clock → ring.
  - `NjEncoder.{hpp,cpp}` — OGG-Vorbis encode of local input intervals for upload.

## Adding a module

1. `src/<Name>.cpp`: a `Module` subclass + `ModuleWidget` subclass +
   `Model* model<Name> = createModel<...>("<Name>");`.
2. `extern Model* model<Name>;` in `src/plugin.hpp`; `p->addModel(model<Name>);` in
   `src/plugin.cpp`.
3. `res/<Name>.svg` panel (mm; 1 HP = 5.08 mm wide, 128.5 mm tall) + a `modules[]` entry
   in `plugin.json`.

## Persistence

Per-instance state via `dataToJson`/`dataFromJson` (Radio persists `url`/`stationName`/
`icon`/`playing` and auto-resumes; Ninjam persists `mode`/`joined` and a LISTEN
`roomLabel`, then auto-resumes — but **never** `transmitting`, so a loaded patch may
rejoin a room yet never auto-broadcasts the user's live input; TX is always a fresh click). The slug strings in `plugin.json` and
`createModel(...)` are permanent identity — never rename once patches reference them.

**NINJAM credentials never go in a patch.** The join server host/port, username, and
password are persisted **only** in the global file `asset::user("akaudio-ninjam.json")`
(`0600`), as an ordered, per-server keyed store (`servers[]`, most-recent-first, cap 12,
recalled by `host:port` in the panel dropdown). `dataToJson` deliberately omits all of it
(and omits `roomLabel` in JOIN mode, since it echoes the private host), so a shared `.vcv`
leaks no credentials. On patch load the module reconnects to the local default server from
that file, not one named by the patch. The old flat single-credential format is migrated
on first load.

## Stations = factory presets (Radio)

Radio's "stations" are **factory presets**, not a bespoke DB — we lean on Rack's
native preset system. Each `.vcvm` is a module `toJson()` whose `data` is `dataToJson`
(`url`, `stationName`, `icon`, `playing:true`). Rack lists them under right-click →
**Preset**; we also surface them via an on-panel `StationChoice` and a context-menu
**Stations** submenu, both loading through `ModuleWidget::loadAction` (real preset loader,
with undo).

- **Theme (deliberate): ambient/utility sound *sources*, not music** — Radio feeds a
  patch, so a fixed-tempo song fights it. Categories: Nature & Ambient, Space & Science,
  Scanners & ATC, News & Talk, Spoken & Stories (see `presets/Radio/`).
- **Liveness is mandatory** — niche feeds rot; confirm `audio/mpeg` (or AAC/HLS) live
  before shipping (drive the URL through `test/play_test.cpp` and check it decodes real
  audio, not just connects). `tools/check_stations.sh` sweeps **all** bundled presets
  through play_test (run before a Library release); it reports scanner feeds that
  decode but are silent this window as `~ silent-but-alive`, not failures.
- **Add a station = drop a verified `.vcvm` in `presets/Radio/<Category>/`** (named
  `NN_<Name>.vcvm`). `appendStationDir()` recurses subfolders → submenus, mirroring
  Rack's factory-preset folder convention. At runtime, users add their own by pasting a
  URL (the importer writes a `.vcvm` to the user preset dir → shows under "Your stations").
- **Artwork:** `icon` is either a plugin-relative bundled PNG (`res/stations/<id>.png`,
  256²) or, for importer-fetched favicons, a portable `cache:<file>` reference into the
  user favicon dir (`asset::user("akaudio-stations")`). `drawStationArt` resolves all
  three: `cache:` → the favicon dir, a leading `/` → a legacy absolute path (older saved
  patches), else `res/`. **Never persist an absolute favicon path** — it embeds the user's
  home dir (account name) and would leak in a shared patch/preset; `portableIcon()`
  normalizes the importer's absolute path to `cache:<file>` before it's stored. NanoVG
  decodes png/jpg/gif/bmp but **not .ico** (so `ImageCache` converts those). Rendered on
  the panel (`StationArt`) and as picker thumbnails (`StationItem`, green ring = current);
  no usable art → synth avatar.

## Test harnesses (`test/`, no Rack link)

- `play_test.cpp` — drives `StreamClient` against a live URL, reports decoded-audio stats.
  `StreamClient` now pulls in the TLS/HTTP/HLS/AAC layers, so the link needs them plus
  OpenSSL (the `libRack` dep's static libs work for a standalone test) and, on macOS, the
  AAC frameworks:
  ```bash
  c++ -std=c++11 -I src -I $RACK_DIR/dep/include test/play_test.cpp \
    src/net/Stream.cpp src/net/Http.cpp src/net/Tls.cpp src/net/Hls.cpp src/net/AacDecoder.cpp \
    src/net/Socket.cpp src/net/Log.cpp \
    src/dep/dr_mp3_impl.cpp \
    $RACK_DIR/dep/lib/libssl.a $RACK_DIR/dep/lib/libcrypto.a \
    -framework AudioToolbox -framework CoreFoundation \
    -o build/play_test && build/play_test [url] [seconds]
  ```
- `njclient_test.cpp` — NINJAM protocol client against a server.
- `enc_test.cpp` — OGG-Vorbis encoder.

## Publishing to the VCV Library

The official library is **build-from-source**: VCV's farm compiles your repo for **all
four targets** (mac-x64, mac-arm64, lin-x64, win-x64) with the
[rack-plugin-toolchain](https://github.com/VCVRack/rack-plugin-toolchain). You submit a
*source URL + commit*, not binaries. So the gating requirement is **it must build *and
link* on every platform**, not just macOS.

- **Cross-platform build is the real hurdle (verify before submitting).** Run all targets
  via the toolchain (Docker; ~15 GB disk, 8 GB RAM): `make plugin-build` against this repo.
  Known risk areas:
  - **TLS/OpenSSL (`src/net/Tls.cpp`)** resolves `SSL_*`/SHA1 from `libRack`'s OpenSSL
    exports. macOS uses `-undefined dynamic_lookup` (deferred to load time); Linux resolves
    undefined `.so` symbols at `dlopen`; **Windows DLLs must resolve every symbol at link
    time** against an import lib. This was flagged as the most likely blocker — but
    **verified clear (2026-06-30): `libRack.dll.a` re-exports the OpenSSL symbols**, so the
    Windows link resolves TLS with no fallback needed. (Fallbacks if a future SDK drops them:
    `ifdef` networking off on Windows, bundle a small TLS lib, or route through Rack's
    `network.hpp`/libcurl.)
  - **Windows sockets = Winsock2, not POSIX.** The net/ layer's BSD-socket calls are mapped
    onto Winsock2 by `src/net/Socket.{hpp,cpp}` (a thin compat shim: `netClose`/`netShutdown`/
    `netSetNonBlocking`/timeout/`netWouldBlock`/`netConnectInProgress`/`netErrorStr`, plus
    `char*` buffer casts and `WSAStartup` from `init()`). The Makefile adds `-lws2_32`
    (`ifdef ARCH_WIN`). Keep new socket code going through the shim, not raw POSIX calls.
  - **macOS-only AAC/HLS** (`AacDecoder`/`Hls`, AudioToolbox) are `ifdef ARCH_MAC`-guarded
    in the Makefile and degrade gracefully off-mac (no AAC/HLS, MP3 still works) — compiles
    everywhere (verified on Windows).
- **Manifest/version rules** the library enforces (see `plugin.json`): `slug` is permanent
  identity (only `[A-Za-z0-9_-]`, never rename post-release); `version` is
  `MAJOR.MINOR.REVISION` with **MAJOR = Rack major (2)**, no `v` prefix; `sourceUrl` is the
  repo homepage (not the `.git` URL); `license` is an SPDX id (ours `GPL-3.0-or-later` —
  required to be GPLv3 anyway since the TLS code links Rack's OpenSSL).
- **Ethics guidelines** (VCV reviews these): no cloning the brand/logo/**panel design or
  component layout** of an existing product without permission, and no harming the user's
  computer or **privacy**. Two things to be ready to defend for this plugin: (1) Radio's
  panel uses VCV's standard **AUDIO** control vocabulary (its own house style, not a
  third-party product's design); (2) all network access is user-initiated
  (chosen stream/room URLs, radio-browser lookups, favicon fetches) with **no telemetry** —
  worth a one-line privacy note in the README. A reviewer may also note TLS uses
  `SSL_VERIFY_NONE` (certs not verified).
- **Submission (open-source):** make the repo public, then open **exactly one issue** on
  [VCVRack/library](https://github.com/VCVRack/library) with the **title = slug (`akaudio`,
  not "AK Audio")** and the source URL in the body. A maintainer reviews + builds + publishes.
- **Every later release:** bump `version` in `plugin.json`, commit, push, then **comment on
  that same issue** with the new version and the exact **commit hash** (`git rev-parse
  HEAD`) — never a branch name. (A git tag like `v2.0.0` is good hygiene but the library
  keys off the commit hash, not the tag.)
- Commercial/closed-source instead: email `contact@vcvrack.com`.
