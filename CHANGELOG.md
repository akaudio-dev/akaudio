# Changelog

All notable changes to **AK Audio** are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow VCV Rack's
scheme (`MAJOR.MINOR.REVISION`, with `MAJOR` = the Rack major version).

## [2.0.8] — 2026-08-29

### Added

- **Looper** — a new module (**beta**): an 8×8 multi-channel always-on looper that
  runs on the NINJAM jam clock (a Ninjam expander; a simulated clock stands in when
  playing solo). Launch, stop, and recording start/finish are quantized to the
  **beat**; takes can be any whole-beat length up to one interval; playback
  free-runs, so tempo changes never touch committed audio. Pickup capture (what you
  play before the starting beat folds into the loop's tail), auto-advance chains
  with press-to-finish, per-cell repeats/decay/follow actions, scenes, continuous
  overdub, and two-way track-name sync with a MindMeld MixMaster. Every take is
  saved to disk as raw OGG (overwrites retired into `history/`, never deleted), the
  grid restores on patch reload, and takes can embed in the `.vcv` itself.
- **Recorder** — a new module (**beta**): the jam's wire archive. Sitting armed next
  to a joined Ninjam, it writes every player's received intervals and your
  transmitted mix to disk as the raw OGG bytes (no re-encode) with a session-timeline
  index — and when recording stops it builds an **Ableton Live set** of the whole
  jam: the Looper grid as Session clips, as-played Arrangement lanes, every player on
  the timeline. Targets Live Standard/Suite (a track per player) or Live Lite
  (fitted into the 8-track cap).
- **Ninjam**: connection failures now say *why* on the panel — cannot resolve /
  refused / timed out / unreachable — matched by a troubleshooting guide in the new
  user manual (`docs/MANUAL.md`, with per-panel references for all modules).

### Fixed

- **Windows**: all connections could fail with "socket error 10022" once the
  process's socket handles grew large (e.g. after long HLS radio sessions) — the
  `fd_set` value-bound check is POSIX-only now.

## [2.0.7] — 2026-08-12

### Internal

- Fixed the VCV Library pattern-check findings from the 2.0.6 review: Ninjam's
  transport/jam views no longer draw blank in the module browser and website previews
  (`module == nullptr`), and Radio's station art loads through a recognized per-frame
  image-cache helper (that finding was benign — the load was already per-frame).

## [2.0.6] — 2026-08-08

### Added

- **MIDI-mappable transport** — Radio's play/stop and Ninjam's metronome are now
  MIDI-Map/CAT-learnable params (visuals unchanged), with a context-menu toggle between
  LATCH (default) and MOMENTARY for note/pad triggers.

### Internal

- CodeQL scanning plus repeatable static-analysis/TSan sweeps; fixed all cppcheck and
  clang-tidy findings across `src/`.

## [2.0.5] — 2026-07-31

### Added

- **Dark panels.** Both modules follow Rack's *View → Use dark panels*, with the same
  inversion convention as VCV Fundamental — near-black panels, light text, and the
  Radio output plate flipping to a light plate with dark labels. The Ninjam chat
  console deliberately stays dark in both themes, like Rack's LED displays.
- **Global display name for public jams.** A **"you: \<name\>"** chip beside the room
  filter (also in the module context menu) sets the one name used for every anonymous
  public-room join; the chat prompt shows your session identity. The name lives only in
  the local settings file — never in a patch — and applies at the next join (NINJAM
  fixes the name at login).

### Fixed

- Clicking a public room in the browser now always joins **anonymously** with the
  display name, instead of silently inheriting whatever username/password the private
  server card last held. A stale registered password there used to make the click
  appear to do nothing (the login failed) and then mis-filed the private credentials
  under the public server's address. Joining anonymously also no longer overwrites a
  stored registered login.

## [2.0.4] — 2026-07-25

Includes the unreleased 2.0.2/2.0.3 work. Ninjam's sync engine was rebuilt for latency,
and AAC/HLS went cross-platform.

### Added

- **AAC and HLS on Windows and Linux** (via vendored FAAD2, full HE-AAC v2) — formerly
  macOS-only. BBC Radio 4 and friends now play everywhere.
- **Voice mode** (Ninjam context menu): rolling ~2 s chunks for near-live, unsynced
  talkback alongside the beat-synced jam.
- `tools/install_win.ps1`: one-command build + install on Windows (MSYS2/MINGW64).

### Changed

- **Much lower Ninjam latency.** Receive playout is arrival-locked (uniform ~one-interval
  latency instead of one-to-two); transmit streams each interval while it is being
  captured, like the canonical njclient.
- **The join gap is bridged.** Until a channel's interval chain locks, its in-flight
  interval plays as a live preview (~1 s behind the sender) and hands over seamlessly;
  the transport bar counts down "audio in ~Xs".
- Transmit now works in rooms whose interval exceeds the capture ring, and never
  auto-resumes on patch load — a pulsing **START TRANSMITTING** nudge appears instead
  when you sit silent in a room with an instrument plugged into IN.

### Fixed

- Linux crash (SIGSEGV) the moment any AAC/HLS stream started — FAAD2's internal FFT
  symbols collided with `libRack.so` exports; now compiled with hidden visibility.
- Replaced rotted bundled stations.

## [2.0.1] — 2026-07-18

A hardening pass across the whole network layer — security, privacy, and reliability —
ahead of wider distribution.

### Security

- Redirects that resolve to private/loopback/link-local addresses (SSRF) and any
  `https → http` downgrade are refused, for both audio streams and small fetches.
- URLs containing raw control bytes are rejected (playlist/redirect header injection);
  logged and displayed URLs are redacted of credentials and query strings.
- Hardened parsers and buffers: ICO palette/offset overflow clamp, chat-line length cap
  against hostile-server memory bloat, `FD_SET` overflow guard.

### Privacy

- NINJAM credentials moved out of patches into a local per-server store
  (`akaudio-ninjam.json`, 0600) — a shared `.vcv` leaks nothing.
- Favicons are stored as portable `cache:<file>` references, never absolute paths that
  embed the account's home directory.
- Transmit never auto-resumes on patch load; the ninbot room-directory fetch is strictly
  user-initiated; networking initializes lazily on first connect, not at plugin load.

### Changed

- Faster, sturdier connects: parallel candidate racing (happy eyeballs), abortable
  DNS/TLS, bounded idle timeouts, and failures-only diagnostics in Rack's `log.txt`.
- Both modules surface stream errors on the panel (reason text instead of a stuck LED
  or station art).

### Fixed

- Windows exit hangs, zombie sessions on rapid station/room switches, an audition
  state-machine race, a draw regression, and several rotted bundled stations
  (redirects, raw-AAC HLS, Radio France URL scheme).

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

[2.0.6]: https://github.com/akaudio-dev/akaudio/releases/tag/v2.0.6
[2.0.5]: https://github.com/akaudio-dev/akaudio/releases/tag/v2.0.5
[2.0.4]: https://github.com/akaudio-dev/akaudio/releases/tag/v2.0.4
[2.0.1]: https://github.com/akaudio-dev/akaudio/releases/tag/v2.0.1
[2.0.0]: https://github.com/akaudio-dev/akaudio/releases/tag/v2.0.0
