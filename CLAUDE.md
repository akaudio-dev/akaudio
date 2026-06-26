# CLAUDE.md — AK Audio VCV Rack plugin

Guidance for Claude Code working in this repo. See `../CLAUDE.md` for the surrounding
workspace (the source-built Rack SDK at `../Rack`; never touch the user's Rack Pro app).

## What this is

A single VCV Rack **plugin** (slug `akaudio`) with multiple **modules** — the Bidoo model:
one slug, one shared library, one Library page, many modules. Both modules are
"network audio → Rack engine" and share `src/net/`.

- **Radio** (`src/Radio.cpp`) — streaming internet radio source (Icecast/HTTP, MP3 /
  AAC / HLS). Ships curated factory presets (see "Stations" below). Panel matches VCV
  core/Fundamental exactly (it deliberately mirrors the **AUDIO** module): `ebebeb→e1e1e1`
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

```bash
export RACK_DIR=/Users/akozlov/work/VCV/Rack   # or rely on Makefile default ../Rack
make                 # -> plugin.dylib
make install         # package + install into ~/Library/Application Support/Rack2/plugins-mac-arm64/
make clean
```

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
  MP3 mount (ssl_stream ignored).
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
`icon`/`playing` and auto-resumes; Ninjam persists last server/credentials/room). The slug
strings in `plugin.json` and `createModel(...)` are permanent identity — never rename once
patches reference them.

## Stations = factory presets (Radio)

Radio's "stations" are **curated factory presets**, not a bespoke DB — we lean on Rack's
native preset system. Each `.vcvm` is a module `toJson()` whose `data` is `dataToJson`
(`url`, `stationName`, `icon`, `playing:true`). Rack lists them under right-click →
**Preset**; we also surface them via an on-panel `StationChoice` and a context-menu
**Stations** submenu, both loading through `ModuleWidget::loadAction` (real preset loader,
with undo).

- **Theme (deliberate): ambient/utility sound *sources*, not music** — Radio feeds a
  patch, so a fixed-tempo song fights it. Categories: Nature & Ambient, Space & Science,
  Scanners, ATC, News & Talk, Spoken & Stories (see `presets/Radio/`).
- **Liveness is mandatory** — niche feeds rot; confirm `audio/mpeg` (or AAC/HLS) live
  before shipping. `tools/gen_stations.py` helps generate presets.
- **Add a station = drop a verified `.vcvm` in `presets/Radio/<Category>/`** (named
  `NN_<Name>.vcvm`). `appendStationDir()` recurses subfolders → submenus, mirroring
  Rack's factory-preset folder convention. At runtime, users add their own by pasting a
  URL (the importer writes a `.vcvm` to the user preset dir → shows under "Your stations").
- **Artwork:** `icon` is either a plugin-relative bundled PNG (`res/stations/<id>.png`,
  256²) or, for importer-fetched favicons, an **absolute path** in the user favicon cache
  (`drawStationArt` treats a leading `/` as absolute). NanoVG decodes png/jpg/gif/bmp but
  **not .ico** (so `ImageCache` converts those). Rendered on the panel (`StationArt`) and
  as picker thumbnails (`StationItem`, green ring = current); no usable art → synth avatar.

## Test harnesses (`test/`, no Rack link)

- `play_test.cpp` — drives `StreamClient` against a live URL, reports decoded-audio stats.
  `c++ -std=c++11 -I src test/play_test.cpp src/net/Stream.cpp src/dep/dr_mp3_impl.cpp -o build/play_test && build/play_test [url] [seconds]`.
- `njclient_test.cpp` — NINJAM protocol client against a server.
- `enc_test.cpp` — OGG-Vorbis encoder.
