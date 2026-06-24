# TODO

Only open/future work lives here. Shipped features are documented in `CLAUDE.md`,
not tracked here. Add stations by dropping a verified `.vcvm` in
`presets/Radio/<Category>/` (see `tools/gen_stations.py`).

## Radio

- **Live curated DB (richer than bundled presets).** Optionally browse
  radio-browser.info at runtime (its `/json/stations/search` API: name, url, codec,
  bitrate, tags, country, popularity, last-check-ok). It is the canonical source
  (confirmed: RadioUnit relies on it entirely). **Liveness validation is mandatory** —
  scraped URLs rot; re-check before showing/shipping, drop dead ones. Likely shape:
  bundle the current preset snapshot + an optional online refresh that generates `.vcvm`
  presets and/or a `res/stations.json`. Data is CC0/free — verify attribution terms.
- **Runtime favicon fetch.** For arbitrary (non-bundled) stations: download the
  radio-browser `favicon` off-thread via `httpGet` → `nvgCreateImageMem` on the UI
  thread → disk cache; filter to PNG/JPG (NanoVG can't decode `.ico`). Bundled presets
  already carry `res/stations/<id>.png` art.
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
Remaining polish:
- Chat: own-vs-others colour, scrollback, message wrap.
- Per-channel TX gain + name UI (multiple local channels).
- Room browser: per-row tooltips (country/city from `users[]`), keyboard nav, persist
  scroll/filter, dim non-playable (TLS-only) rooms instead of hiding them.

## Parked

- **LiveATC** — `.pls` resolves but the feed blocks non-browser clients (would need
  UA-spoofing). ATC is instead served via Broadcastify feeds (plain MP3): add one by
  dropping a `.vcvm` pointing at `broadcastify.cdnstream1.com/<id>` in
  `presets/Radio/ATC/`. ATC is push-to-talk — verify over a ~30 s window (short windows
  hit silent gaps).
