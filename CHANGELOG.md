# Changelog

All notable changes to **AK Audio** are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow VCV Rack's
scheme (`MAJOR.MINOR.REVISION`, with `MAJOR` = the Rack major version).

## [2.0.0] — 2026-06-30

First public release: two network-audio modules sharing a common streaming, decode, and
lock-free ring-buffer layer.

### Radio

- Streaming internet-radio source for Icecast/HTTP streams: **MP3** everywhere, plus
  **AAC** and **HLS** (`.m3u8`) on macOS.
- Built-in **VCA** on the LEVEL knob using VCV's AUDIO taper (−∞…+12 dB) with a gauge
  ring, and an optional unipolar 0–10 V **CV** input.
- Stereo output (LEFT/RIGHT) on a Fundamental-style panel.
- **Factory station presets** grouped by theme — Nature & Ambient, Space & Science,
  Scanners & ATC, News & Talk, Spoken & Stories — with a ▲/▼ stepper and an on-panel
  picker.
- **Add a station from a URL:** paste a stream URL and it auditions the stream (verifying
  real audio actually flows), identifies it via [radio-browser.info](https://www.radio-browser.info),
  downloads and caches its favicon, and saves it as a user preset — with a failed
  audition rolling back cleanly.
- Auto-resolves `.pls`/`.m3u` playlists and follows redirects.

### Ninjam

- **LISTEN:** play a room's public Icecast/HTTP mix — no protocol, no login.
- **JOIN:** the full NINJAM protocol — connect, anonymous SHA1 auth, decode the live
  multi-user OGG interval mix, and **transmit** your input jacks (downbeat-aligned
  OGG-Vorbis encode) so other participants hear you.
- **Room chat** (send and receive).
- In-panel **room browser** fed by ninbot's public directory: search, scrollable list,
  click to listen or join, and a peak meter. The UI never blocks on the network.

### Platforms

- Builds for macOS (arm64/x64), Linux (x64), and Windows (x64). AAC/HLS are macOS-only;
  MP3 streaming and the full NINJAM path work on every platform.

### Privacy

- No telemetry, analytics, tracking, or accounts. All network access is user-initiated
  (chosen streams/rooms, radio-browser lookups, favicon fetches). See the
  [README](README.md#privacy) for the full breakdown, including that JOIN transmits your
  input audio.

[2.0.0]: https://github.com/akaudio-dev/akaudio/releases/tag/v2.0.0
