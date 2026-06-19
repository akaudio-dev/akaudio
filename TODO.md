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
