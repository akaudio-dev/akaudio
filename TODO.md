# TODO

Only open/future work lives here. Shipped features are documented in `CLAUDE.md`,
not tracked here. Add stations by dropping a verified `.vcvm` in
`presets/Radio/<Category>/` (verify liveness with `test/play_test.cpp` first).

## VCV Library submission (active milestone)

Goal: get `akaudio` into the official VCV Library. The library builds from source on **all
four targets** (mac-x64, mac-arm64, lin-x64, win-x64), so the blocker is cross-platform
build, not packaging. Process + rules are in `CLAUDE.md` → "Publishing to the VCV Library".

- [x] **Verify the Linux build** — done 2026-06-29. `plugin.so` builds cleanly against
      SDK 2.6.4; `SSL_*`/`EVP_sha1` symbols correctly defer to `libRack.so` at dlopen.
      Standalone tests pass: `enc_test` (OGG round-trip), `play_test` (NPR HTTPS MP3),
      `njclient_test` (NINJAM auth + roster from ninbot.com:2051).
- [x] **Verify the Windows build (the likely blocker)** — done 2026-06-30. Builds, links,
      and packages cleanly: `plugin.dll` + `akaudio-2.0.0-win-x64.vcvplugin` against SDK
      2.6.4 with the MSYS2 mingw-w64 GCC toolchain. **The feared OpenSSL blocker did not
      materialize:** `libRack.dll.a` re-exports `SSL_*`/SHA1, so TLS resolved at link time
      with no fallback needed — only `-lws2_32` had to be added (Makefile `ifdef ARCH_WIN`).
      The net/ layer was ported from POSIX sockets to Winsock2 behind a compat shim
      (`src/net/Socket.{hpp,cpp}`): handle close/shutdown, non-blocking via `ioctlsocket`,
      `SO_RCVTIMEO` as a DWORD, `WSAGetLastError`-based would-block/in-progress predicates,
      `char*` buffer casts, `WSAStartup` in `init()`, and `#ifdef SIGPIPE` in plugin.cpp.
      `mkdir` made portable in `ImageCache.cpp` (`_mkdir`). AAC/HLS still mac-only (stubbed
      off-mac). Runtime load in Rack on Windows not yet smoke-tested.
- [x] **Run `rack-plugin-toolchain` (lin-x64 + win-x64)** — done 2026-06-30 on a native
      x86-64 Linux box (Docker; the Dockerfile was trimmed to skip `toolchain-mac`, which
      dropped the osxcross/LLVM compile). Both targets build, link, and package cleanly via
      the real toolchain against SDK 2.6.6: `akaudio-2.0.0-lin-x64.vcvplugin` (1.18 MB) and
      `akaudio-2.0.0-win-x64.vcvplugin` (1.20 MB). **Windows OpenSSL/Winsock confirmed on
      the actual toolchain:** `x86_64-w64-mingw32-g++ ... -lRack ... -lws2_32` links with no
      undefined `SSL_*`/SHA1 references. The **mac-x64/arm64** toolchain targets were skipped
      on purpose — they need Apple's non-redistributable macOS SDK, mac-arm64 builds natively
      here (primary platform), and VCV's farm compiles all four on submission anyway.
- [x] **README privacy note** — done 2026-06-30. README `## Privacy` split into incoming
      vs outgoing, making NINJAM JOIN's audio/chat transmit explicit; names `SSL_VERIFY_NONE`
      and clarifies favicon fetch. **Decision: keep `SSL_VERIFY_NONE`** for now (public audio
      /jamming, not credentials; enforcing verification means bundling a CA store for little
      gain) — documented honestly so a reviewer sees it.
- [x] **Manifest polish** — done 2026-06-30. `manualUrl` → `docs/MANUAL.md` (new user
      manual), `changelogUrl` → `CHANGELOG.md` (new, Keep-a-Changelog 2.0.0 entry).
      `donateUrl` intentionally left empty.
- [ ] **Flip the repo to public**, then open **one** issue on `VCVRack/library` titled
      `akaudio` (the slug, not "AK Audio") with the source URL.

## Radio

- **Add-station-from-URL — shipped.** Paste a stream URL in the context menu → it
  auditions (verify real audio → identify via radio-browser `byurl` → fetch favicon →
  save a user preset). Favicon fetch + `.ico` parsing are done (`net/ImageCache`,
  `net/StationImport`). Remaining ideas: a fuller *browse* of radio-browser's
  `/json/stations/search` (a previous in-panel list was tried and reverted — 8 HP is too
  small; would need a different surface), and SVG-favicon support (radio-browser often
  serves SVG logos, which we currently skip → synth avatar; could rasterize via NanoSVG).
- **Bundled presets rot — periodic re-verify.** A `tools/` liveness sweep (chunk-fetch
  each `.vcvm` URL; treat a streaming mount as live even if momentarily silent, e.g. ATC
  push-to-talk) to prune/refresh dead stations. 10 dead were pruned 2026-06-25.
- **Codec/quality gaps.** No OGG/Vorbis on the *radio* stream path (NINJAM intervals do
  decode OGG via stb_vorbis). AAC + HLS are macOS-only (other platforms fall back to an
  error). Resampling is linear, not band-limited.

## HLS (macOS, `net/Hls.cpp`)

Works for `.ts`/AAC media playlists (BBC Radio 4 etc.). Still wanted:
- **fMP4/`mp4a` segment support** — some feeds use it instead of MPEG-TS; would unblock
  more modern HLS hosts.
- **Master-playlist variant selection by bitrate** (currently picks first variant).
- An HLS unit test.

## Ninjam — UI polish

Protocol JOIN (connect/auth/subscribe/decode) and **transmit** (downbeat-aligned
OGG-Vorbis encode → `UPLOAD_INTERVAL_*`) are both done and verified; chat send/recv works.
The panel is now visually consistent with Radio/Fundamental — dark `#1f1f1f` OUT plates,
`#f0f0f0` labels, "AK" maker mark, and IN/OUT jack rows + plate keyed to Radio's exact mm
grid (input row 96.859 mm, output row 113.115 mm, plate 104.66/13.26 mm, 3.9 mm margins).
Remaining polish:
- Per-channel TX gain + name UI (multiple local channels).
- Room browser: per-row tooltips (country/city from `users[]`), keyboard nav, persist
  scroll/filter, dim non-playable (TLS-only) rooms instead of hiding them.
- Unify NINJAM's left/right content margins: OUT plates now use Radio's 3.9 mm margin
  while the chat/browser cards above still use a 6 px inset — pick one and apply it.

## Parked

- **LiveATC** — `.pls` resolves but the feed blocks non-browser clients (would need
  UA-spoofing). ATC is instead served via Broadcastify feeds (plain MP3): add one by
  dropping a `.vcvm` pointing at `broadcastify.cdnstream1.com/<id>` in
  `presets/Radio/Scanners & ATC/` (the ATC and Scanners categories were merged). ATC and
  scanner feeds are push-to-talk — verify over a ~30 s window (short windows hit silent
  gaps), and discard feeds that decode pure digital silence (offline).
