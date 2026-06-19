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
