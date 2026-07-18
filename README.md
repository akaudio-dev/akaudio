# AK Audio

A personal VCV Rack plugin (collection of modules) by Andrei Kozlov.

Modules:

- **Ninjam** — NINJAM online jamming client; streams collaborative audio from a NINJAM server into Rack. (Port of the ideas in the `jamauv3` AUv3 project to a Rack module.)
- **Radio** — streaming internet radio (Icecast/HTTP) source feeding audio into a patch.

Both modules share the networked-audio layer in `src/net/` (HTTP/Icecast streaming, codec decode, lock-free ring buffer feeding the audio thread).

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
