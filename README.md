# AK Audio

A personal VCV Rack plugin (collection of modules) by Andrei Kozlov: network audio in
and out of Rack — internet radio, live NINJAM jamming, an interval looper built for
jamming, and a recorder that turns whole jams into Ableton Live sets. All modules share
the networked-audio layer in `src/net/` (HTTP/Icecast streaming, codec decode,
lock-free ring buffer feeding the audio thread). Full user manual: [docs/MANUAL.md](docs/MANUAL.md).

## Modules

### Radio

![Radio](docs/images/Radio.png)

Streaming internet radio as a patch source. Point it at any Icecast/HTTP stream (MP3,
AAC, HLS) and it decodes on a background thread and feeds your patch through a built-in
level control — with a curated set of ambient, spoken-word, and scanner stations
bundled as presets, and one-paste import of your own stream URLs (verified live,
identified, and given artwork before anything is saved).
[Manual →](docs/MANUAL.md#radio)

### Ninjam

![Ninjam](docs/images/Ninjam.png)

A NINJAM online-jamming client in a Rack module. LISTEN to a room's public stream with
zero setup, or JOIN with the real protocol: hear the live multi-user mix (arrival-locked
for a uniform one-interval latency, with a live preview bridging the join gap) and
transmit your own instruments, streamed interval-by-interval like the canonical client.
In-panel room browser, room chat, and a voice mode for talkback.
[Manual →](docs/MANUAL.md#ninjam)

### Looper

![Looper](docs/images/Looper.png)

An 8×8 Ableton-Session-style interval looper that runs on the jam's own clock (an
expander of Ninjam — every capture, launch, and stop commits exactly on the interval
boundary; a simulated clock stands in when playing solo). Always-on rolling recording
makes committing a take a pointer move: press an empty cell and the next interval is
yours, keep playing through the downbeat and the recording rolls into the next cell and
wires itself into a replayable chain. Per-cell repeats, decay, and follow actions;
scenes; continuous overdub; tempo-change survival (takes re-pitch tape-style or re-tile
across BPI changes); two-way track-name sync with a MindMeld MixMaster; and every take
is saved to disk and restored with the patch.
[Manual →](docs/MANUAL.md#looper)

### Recorder

![Recorder](docs/images/Recorder.png)

The jam's black box, and the way out of Rack: as a Ninjam expander it archives every
player's intervals and your transmitted mix to disk as the raw OGG bytes — no re-encode,
nothing lost — and when recording stops it automatically reassembles the whole session
as an **Ableton Live set**: the Looper grid in Session view, everyone's audio and your
as-played loop timeline laid out in the Arrangement, tempo and loop braces set. A menu
switch targets full Live (a track per player) or Live Lite's 8-track cap (players
merged onto one lane).
[Manual →](docs/MANUAL.md#recorder)

## Building

This plugin builds against a sibling source build of Rack at `../Rack`.

```bash
export RACK_DIR=/path/to/Rack   # or rely on the Makefile default ../Rack
make            # -> plugin.dylib
make install    # package + install into the Rack user plugins folder
make clean
```

## Adding a module

1. Add `src/<Name>.cpp` (Module + ModuleWidget + `Model* model<Name> = createModel<...>("<Name>")`).
2. Declare `extern Model* model<Name>;` in `src/plugin.hpp`.
3. Register it with `p->addModel(model<Name>);` in `src/plugin.cpp`.
4. Add a `res/<Name>.svg` panel and a module entry in `plugin.json`.

## Privacy

AK Audio makes network connections **only when you ask it to**, and only to the servers
needed to play or share what you choose. There is **no telemetry, no analytics, no
tracking, and no account** — nothing is collected, and nothing leaves your machine
except the connections listed below, only while a module is active.

**Incoming only** (the plugin receives audio; it sends nothing but the request):

- **Radio / Ninjam (LISTEN)** — connects to the stream URL you pick (or a bundled station
  preset) and plays its audio.
- **Add a station from a URL** — looks the URL up on
  [radio-browser.info](https://www.radio-browser.info) to fetch the station's real name,
  then downloads its icon from the station's own server. The icon is cached as a file in
  your Rack user folder; nothing about you is uploaded.
- **Room browser** — fetches the public list of NINJAM rooms from ninbot.com **only when
  you open the list** (hit Refresh, click into it, or type in the filter). Simply adding a
  Ninjam module or opening a patch that contains one never contacts ninbot on its own.

**Outgoing — this sends your audio and text to a server and other people:**

- **Ninjam (JOIN)** — connects to the NINJAM server you choose (anonymous login) and
  **transmits the audio on the module's input jacks** so other participants in the room
  can hear it, in real time. Any **chat** messages you send go to the same server. Only
  use JOIN when you intend to be heard; LISTEN never transmits. Transmitting is always an
  explicit choice you make each session — loading a patch never starts broadcasting on its
  own, even if it was saved while transmitting.

**Your credentials stay on your machine.** A NINJAM server login (username and password)
is saved only in a local file in your Rack user folder (owner-readable only), **never
written into a saved patch** — so sharing a `.vcv` patch never leaks your password or the
list of servers you've joined. Registered-server passwords are protected on the wire by
NINJAM's challenge-response, but the NINJAM protocol itself is unencrypted, so treat room
chat and audio as public and don't reuse a valuable password on a NINJAM server.

Stream and server connections use TLS when the server offers it, but server certificates
are **not currently verified** (`SSL_VERIFY_NONE`) — a deliberate choice appropriate for
public audio and jamming (no passwords travel over these connections), not for anything
sensitive. Redirects to private/internal addresses and TLS-stripping downgrades are
refused.

## License

Copyright © 2026 Andrei Kozlov.

AK Audio is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See [LICENSE](LICENSE) for the full text.

Bundled third-party code retains its own (GPL-compatible) license:
libogg / libvorbis (BSD), stb_vorbis and dr_mp3 (public domain).
