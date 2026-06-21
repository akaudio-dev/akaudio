# TODO

## Curated, auto-maintained streaming-station database

Build a really nice, curated, up-to-date database of streaming audio URLs that the
**Radio** module can browse (instead of the user hand-typing a URL), plus tooling to
keep it fresh automatically. **Reuse existing public lists rather than hand-rolling.**

### Reuse these existing sources
- **radio-browser.info** — large community database with a free JSON API (station name,
  url, codec, bitrate, tags, country, click/vote popularity, last-check-ok flag). Best
  primary source; already does liveness checking.
- **Icecast / Xiph YP directory** (`dir.xiph.org`) — public Icecast directory.
- **SomaFM** — has a clean channel/playlist listing (good, reliable MP3 streams; our
  current default station is SomaFM Groove Salad).
- Optionally curated genre lists (e.g. SomaFM, FIP/Radio France, college radio) for a
  hand-picked "featured" set on top of the big auto-imported set.

### Auto-maintenance approach
- A generator script (Python, alongside `helper.py`-style tooling) that:
  1. Pulls from the sources above via their APIs.
  2. Filters to what the module can actually play — **v1: http:// + MP3 only** (see
     `src/net/Stream.cpp` limitations); widen as we add TLS/AAC/OGG.
  3. Validates each stream is reachable / currently online (radio-browser already flags
     this; re-check a sample), drops dead ones.
  4. Dedupes, normalizes, sorts by popularity, tags by genre/country.
  5. Emits a versioned `res/stations.json` shipped with the plugin.
- Run it periodically (could be a scheduled job / CI) so the shipped DB stays current;
  bump plugin version on refresh. Consider a small "featured/curated" override file that
  is human-maintained and merged on top of the auto-imported set.

### Module integration
- Radio context menu (or a panel browser): pick from the DB by genre/country/search,
  with the editable URL field as the manual fallback (already implemented).
- Persist the selected station; keep the manual-URL path working.

### Pivot 2026-06-20 — ambient/utility sources, grouped by category
The curated set is **not music** (a song fights the patch). It is sound *material* for
soundscapes, grouped into category subfolders (recursive submenus via `appendStationDir`):
**Nature** (Ambi Nature Radio, MyNoise Pure Nature, Nature Radio Rain), **Space** (Blue Mars /
Cryosleep / Voices From Within), **Scanners** (5 Broadcastify police/fire/EMS feeds). All
verified live `audio/mpeg`. Per-station favicons fetched from radio-browser where available
(ambinature, mynoise, broadcastify); Blue Mars/rain fall back to the ♪ placeholder.

Still wanted (next):
- **HLS support — DONE 2026-06-20** (macOS). `net/Hls.{hpp,cpp}` + `StreamClient::runHls`: poll
  the media playlist, fetch `.ts` segments, `tsExtractAdts` (MPEG-TS → AAC-ADTS) → `AacDecoder`.
  Shipped `BBC/`: **BBC Radio 4** + R4 Extra (verified decoding via the harness). World Service
  uses a different HLS host/segment format (fMP4?) and FAILED the TS demux — parked. Possible
  next: fMP4/`mp4a` segment support (for WS and others), master-playlist variant selection by
  bitrate, and an HLS unit test.
- **ATC** — partly solved via **Broadcastify** aviation feeds (plain MP3, same infra as the
  scanner feeds). Shipped `ATC/`: Anchorage Center (ARTCC, feed 31716) + Aeroparque Tower (45890),
  both verified playing (ATC is push-to-talk, so confirm over a ~30 s window — short windows hit
  silent gaps). **LiveATC stays out**: their `.pls` resolves but the feed blocks non-browser
  clients (would need UA-spoofing). To add more ATC, find a Broadcastify feed id
  (`broadcastify.cdnstream1.com/<id>`) and drop a `.vcvm` in `presets/Radio/ATC/`.
- More spoken/world (shortwave WBCQ works as plain MP3), more nature/scanner feeds.

### Superseded 2026-06-20 — stations as factory presets (first cut, was SomaFM music)
Radio now ships curated **factory presets** (`presets/Radio/NN_*.vcvm`, the reliable SomaFM
family, all verified `audio/mpeg`) instead of needing a hand-typed URL. Implemented via Rack's
native preset system: `data` carries `url`/`stationName`/`playing`; an on-panel `StationChoice`
+ a context-menu **Stations** submenu load the chosen file through `ModuleWidget::loadAction`.
`stationName` is persisted and shown on the panel; manual URL edits set it to "Custom". See
CLAUDE.md "Stations = factory presets". This is the simple, idiomatic baseline; the big
radio-browser.info live/curated database above remains the richer future step (and would
generate `.vcvm` presets and/or a `res/stations.json`).

**Station artwork (DONE 2026-06-20, bundle-now half of the hybrid):** each preset carries an
`icon` PNG (`res/stations/<id>.png`, SomaFM 256² logos fetched at build time). Rendered on the
panel (`StationArt`) and as right-aligned thumbnails in the picker (`StationItem`, green ring =
current). NanoVG can't decode `.ico`, so icons must be PNG/JPG. **Deferred (fetch-later half):**
runtime favicon download + cache for arbitrary stations (radio-browser's `favicon` field), via
`httpGet` off-thread → `nvgCreateImageMem` on the UI thread → disk cache; filter to PNG/JPG.

### Open questions
- Ship the full DB vs. fetch/update it at runtime from radio-browser (network at launch
  vs. offline-friendly bundled snapshot — likely bundle a snapshot + optional refresh).
- Licensing/attribution for reused lists (radio-browser data is CC0/free; verify terms).

### Findings from on-system radio AUs (investigated 2026-06-19)
Investigated the two radio Audio Units installed on this Mac:
- **Credland "Radio"** (`net.credland.radio`, AUv2 + standalone) — downloads its directory from
  `radioserver.credland.net` and caches it (zlib `cache.dat`). Extracted **2,832 stations** with
  name/url/category/description → `data/sources/credland_radio_stations.{json,xml}` (gitignored;
  proprietary + low-trust). Useful mainly as a category/curation reference, not as truth.
- **RadioUnit** (`com.giku.RadioUnit` v2.8, AUv3) — thin client over **radio-browser.info**
  (mirrors `at1/de1/de2/fi1/nl1.api.radio-browser.info`, endpoint `/json/stations/search`;
  browses by country via `countries.json`). Only **5 vendor-baked default stations**
  (SomaFM Drone Zone, SomaFM Space Station, Rainwave chiptune, StreamAfrica) →
  `data/sources/radiounit_builtin_defaults.txt`. No user favorites saved.

Conclusions:
- **radio-browser.info is confirmed as the canonical source** (RadioUnit relies on it entirely).
- **Liveness validation is mandatory** — scraped lists are unreliable (observed: Credland's BBC
  Radio 4 URLs are all dead). Treat every imported URL as unverified until checked; re-validate
  periodically and drop dead entries. Do NOT ship a list verbatim from any of these sources.
- **Vendor-baked defaults are a good high-trust seed** for the curated/featured set (SomaFM family
  in particular shows up as a reliable default across both AUs and our own Radio module).

## Ninjam: server picker (staged — start simple, then deluxe)

The Ninjam module listens to a jam's Icecast stream. It needs a UI to pick the room.

**Data source (settled, verified):** `http://ninbot.com/app/servers.php` — JSON `{servers:[…]}`, each
with `host/port/bpm/bpi/name/pri/user_count/user_max/users[]` **and the Icecast `stream` (http MP3)
+ `ssl_stream` (https) URLs** (same endpoint jamauv3's ServerBrowser uses). Verified: serves over
plain http (no TLS needed), `stream` is a plain-http `audio/mpeg` mount our `StreamClient` plays
directly. Refresh ~every 60s, cache-bust with `?t=<epoch>`. Parse with jansson (already linked).

**Architecture (non-negotiable): fetch off-thread, show from cache.** A background `RoomDirectory`
thread fetches+parses into a mutex-guarded `vector<Room>`; the menu is built from the cache
*instantly* (never block the UI thread on the network). Refresh on load, on menu-open (background),
and via a manual "Refresh".

- **Phase 1 — simple (DONE 2026-06-20):** on-panel `LedDisplayChoice` (`NinjamRoomChoice`) shows the
  current room and opens a dynamic menu built from the cache, sorted by user count, each row
  `name   N/max · BPM`, ✓ on current, plus a status line, "Refresh rooms", and the existing
  "Custom URL…" field; same picker is also a "Public rooms" submenu in the context menu. Pick →
  `selectRoom()` sets `stream` URL + starts listening. Implemented: `net/Http.{hpp,cpp}`
  (`httpGet(url, out)`), `net/RoomDirectory.{hpp,cpp}` (background fetch+parse of
  `ninbot.com/app/servers.php`, mutex-guarded cache, sorted snapshot, `?t=` cache-bust), wired into
  `Ninjam.cpp`. No new deps (jansson already linked). Verified live: 19 rooms, `stream` mounts serve
  `audio/mpeg` 128k that `StreamClient` plays directly. `roomLabel` persisted alongside `url`.
- **Phase 2 — deluxe (DONE 2026-06-20):** widened the panel to **20 HP** (101.6 mm) and built an
  in-panel browser in `Ninjam.cpp`: header (title + accent rule + connected light), a `SearchField`
  (`ui::TextField`) that live-filters by room name or player, a `RefreshButton` (↻), a `StatusLabel`
  (directory status), a scrollable `RoomBrowser` (`ui::ScrollWidget`) of `RoomRow`s — each showing
  name, `BPM · BPI · N/max here`, the players list, hover highlight, green accent + ▶/■ glyph on the
  active room; click a row to listen/stop (`Module::toggleRoom`). Footer has a peak level `MeterWidget`
  (atomic `peak`, fast-attack/150 ms release, green→amber→red) and the L/R outputs. The list rebuilds
  only when the directory `generation()` or filter changes; a background refresh fires ~every 30 s
  while visible. The old context-menu picker (`appendRoomMenu`, Custom URL field) is kept as a fallback.
  Verified: clean build/install, model symbol + packaged SVG present. GUI not visually tested here
  (headless: dev Rack aborts at `glfwGetVideoMode`, no monitor) — needs an eyeball pass in Rack.

  Possible polish next: per-row tooltips (country/city from `users[]`), keyboard nav, persist scroll/
  filter, dim non-playable (TLS-only) rooms instead of hiding them, "now playing" marquee.

## Ninjam: JOIN via the NINJAM protocol (studied 2026-06-20 — not yet built)

Today the module only *listens* to a room's public Icecast/MP3 mix. Real participation means
speaking the **NINJAM protocol** (TCP, default port 2049). Studied two reference impls:
`~/work/github/ninjam` (canonical Cockos C++, the `njclient`/`mpb`/`netmsg` spec — **GPLv2**) and
`~/work/github/JamTaba` (mature C++/Qt client, `src/Common/ninjam/` + `NinjamController` — GPLv2).
Both GPL → fine to port into our GPL-3 plugin (keep the protocol classes pure C++; don't drag in Qt).

**Wire format** (verified in canonical source): frame = `[type u8][size u32 LE][payload]`. Strings are
NUL-terminated UTF-8; ints little-endian. `netmsg.cpp` (framing), `mpb.cpp` (message build/parse).

**Auth (SHA1): `passhash = SHA1("user:pass")`, `response = SHA1(passhash + challenge8)`.**
Anonymous = username `anonymous[:displayname]`, empty pass. Confirmed in BOTH references — canonical
`njclient.cpp:1069` and JamTaba `ClientMessages.cpp:47,68-76` (SHA1(user+":"+pass) then
SHA1(that+challenge)). JamTaba is a proven, widely-used client and matches the spec exactly. (An
earlier study-agent summary mis-stated this as "MD5 XOR challenge" — that was the agent's error, NOT
JamTaba's code, which is correct.) We already link OpenSSL via libRack, so SHA1 is free.

**Handshake:** TCP connect → recv `SERVER_AUTH_CHALLENGE(0x00)` (challenge8, server_caps [keepalive secs
in bits 8-15], proto_ver 0x00020000, optional license) → send `CLIENT_AUTH_USER(0x80)` (response20,
username, client_caps [bit0=accept license, bit1 must be set], ver) → recv `SERVER_AUTH_REPLY(0x01)`
(flag bit0=success, maxchan). Then send `CLIENT_SET_CHANNEL_INFO(0x82)` with our channel list (even if
we transmit nothing) and keepalive (`0xfd`) every server_caps interval.

**Minimal LISTEN-only message subset (recv):** `CONFIG_CHANGE_NOTIFY(0x02)` (BPM u16, BPI u16),
`USERINFO_CHANGE_NOTIFY(0x03)` (per-rec: active, chidx, vol dB*10, pan, flags, user, chname),
`DOWNLOAD_INTERVAL_BEGIN(0x04)` (guid16, estsize, **fourcc "OGGv"**, chidx, user; guid all-zero = silence),
`DOWNLOAD_INTERVAL_WRITE(0x05)` (guid16, flags [bit0=last], ogg bytes). Optionally
`CLIENT_SET_USERMASK(0x81)` to subscribe (many servers auto-subscribe).

**Interval audio model:** audio is chunked into intervals aligned to tempo —
`interval_samples = BPI * 60 * sampleRate / BPM`. Per (user,channel) interval: collect the WRITE chunks
by guid → decode **OGG Vorbis** → at the next interval boundary swap it in and mix (vol/pan). A joining
client just syncs to the next boundary; the server enforces alignment.

**Codec dep (we have none):** Rack ships no vorbis/ogg. For **listen-only**, vendor **stb_vorbis.h**
(single-header, public-domain) to decode intervals — mirrors how `dep/dr_mp3.h` works, no build system
changes. For **uploading** later, we'd vendor libvorbis+libogg (heavier; what WDL/JamTaba use).

**Reuse from our code:** the background net thread, `StereoRingBuffer`, linear resampler, and backpressure
in `net/Stream.cpp` all transfer. New: a `NinjamClient` (own TCP socket — NOT `StreamClient`, different
protocol), SHA1 via OpenSSL, a tiny binary reader/writer, per-user interval decoders, and a tempo-driven
interval clock feeding the ring. Threading contract unchanged (decode off-thread, `pull()` on audio thread).

**Gotchas (from JamTaba):** keepalive or the server drops you; intervals are long (~16 s @120/32) so
mid-interval joins wait for the next boundary; no retransmit → validate the "OGGv" header and skip
corrupt intervals; resample remote rate→engine rate; send `SET_CHANNEL_INFO` promptly after auth.

**Plan (listen-first, then transmit):** (1) `NinjamClient` connect+SHA1 auth+keepalive + message
(de)serialize; (2) interval reassembly + stb_vorbis decode + interval-clock → ring (this gives the
*real* multi-user mix, no Icecast needed); (3) wire into the Ninjam module as an alternative to the
Icecast path; (4 later) OGG encode + `UPLOAD_INTERVAL_*` to actually play in the jam. Substantial,
multi-phase. Keep the existing Icecast listening as the zero-dep fallback.

Key references — canonical: `ninjam/{netmsg,mpb,njclient}.{cpp,h}`, `WDL/vorbisencdec.h`;
JamTaba: `src/Common/ninjam/{Ninjam,client/ServerMessages,client/ClientMessages,client/Service}.*`,
`src/Common/NinjamController.cpp`, `src/Common/audio/NinjamTrackNode.cpp`,
and the byte-layout tests `tests/auto/ninjam/TestMessagesSerialization.cpp`.
