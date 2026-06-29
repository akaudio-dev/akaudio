# TODO

Only open/future work lives here. Shipped features are documented in `CLAUDE.md`,
not tracked here. Add stations by dropping a verified `.vcvm` in
`presets/Radio/<Category>/` (verify liveness with `test/play_test.cpp` first).

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
