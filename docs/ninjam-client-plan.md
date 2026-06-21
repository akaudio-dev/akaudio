# Plan — the complete NINJAM client (Ninjam module)

Move the **Ninjam** module from passive Icecast listening to a real **NINJAM protocol
client**: connect to a server, authenticate (anonymous), receive and decode the live
multi-user mix, and — later — transmit our own audio so we actually play in the jam.
Keep Icecast listening as a zero-dependency fallback.

Background + verified protocol facts live in `TODO.md` ("Ninjam: JOIN via the NINJAM
protocol"); references are `~/work/github/ninjam` (canonical Cockos server+client, GPLv2 —
the public servers run this) and `~/work/github/JamTaba` (proven C++/Qt client, GPLv2).

---

## 1. Constraints (non-negotiable)

- **Realtime contract.** `Module::process()` (audio thread) never blocks/allocates/locks.
  All networking and decoding run on background thread(s); the audio thread only `pull()`s
  from a lock-free `StereoRingBuffer`. (Same model as `StreamClient`.)
- **C++11**, macOS arm64 dev build. Sockets are BSD (portable). SHA1 via the **OpenSSL
  that libRack already exports** (we use it for TLS). OGG decode via a vendored single-header.
- **Licensing.** Plugin is GPL-3; references are GPLv2 (compatible). Implement to the
  **canonical `justinfrankel/ninjam` server's** expectations; validate our byte layouts
  against JamTaba's `tests/auto/ninjam/TestMessagesSerialization.cpp` (known-conformant) and
  its `wireshark data/` captures. Credit Cockos/NINJAM + JamTaba in source headers.

## 2. New dependencies

- **`src/dep/stb_vorbis.h`** (public domain, single-header) — OGG **decode** for the mix.
  Vendored exactly like `dep/dr_mp3.h`; one `.cpp` defines the implementation. No build changes.
- **SHA1** — reuse OpenSSL (`<openssl/sha.h>`, already linked via libRack). No vendoring.
- **Encode (transmit phase only, deferred):** libvorbis + libogg (heavier; what WDL/JamTaba
  use). Not needed for listen-only.

## 3. Architecture & files (new: `src/net/ninjam/`)

- **`NjProtocol.hpp/.cpp`** — the wire layer.
  - Frame: `[type u8][size u32 LE][payload]`. A `ByteReader`/`ByteWriter` over
    `std::vector<uint8_t>` (LE ints, NUL-terminated UTF-8 strings, raw bytes).
  - Build/parse the message subset we use (see §5). Pure, unit-testable, no sockets.
- **`NjClient.hpp/.cpp`** — connection + protocol state machine on a background thread.
  - Owns the TCP socket (its own — **not** `StreamClient`, different protocol).
  - `start(host, port, user)` / `stop()` (UI thread; mirror `StreamClient`'s abort+shutdown+join).
  - `run()`: connect → SHA1 auth handshake → loop { recv & dispatch frames; send keepalive }.
  - State + callbacks (no Qt): `onState`, `onConfig(bpm,bpi)`, `onUser(...)`, plus it drives audio.
- **`NjAudio.hpp/.cpp`** (or folded into NjClient) — interval reassembly, decode, mix.
  - `Download` map keyed by 16-byte GUID → accumulates a user/channel's interval bytes.
  - On interval-complete: `stb_vorbis` decode → resample to engine rate (reuse the linear
    resampler from `Stream.cpp`) → buffer per (user, channel).
  - **Mix + emit** (see §4) → `StereoRingBuffer`.
- **Module integration** (`Ninjam.cpp`): a **mode** — *Icecast listen* (current) vs *Protocol
  join* (new) — selected per room. Reuse `RoomDirectory` (already has `host`/`port`/`bpm`/`bpi`)
  to pick and `NjClient::start(host,port)`. Panel shows tempo / roster / interval progress; the
  clickable LED stops it.

## 4. Threading & the interval model (the crux)

NINJAM is **interval-based with one interval of inherent latency**: while interval *N* plays,
the client downloads interval *N+1*. `interval_samples = BPI · 60 · sampleRate / BPM`
(~16 s at 120 BPM / 32 BPI).

**v1 mixing model — pre-mix on the background thread, push stereo to the ring** (matches our
`pull()` model; keeps the audio thread trivial):

- A decode/mix thread keeps, per user/channel, a queue of *completed, decoded* intervals.
- Once per interval slot it gathers each user's next ready interval (silence if none yet),
  applies that user's volume/pan (from USERINFO), mixes to stereo at engine rate, and writes
  `interval_samples` frames into the ring. **Ring backpressure paces playback at realtime** and
  naturally produces the 1-interval cadence — exactly like `StreamClient` streams MP3 today.
- We have a full interval of lead time to decode, so per-user `stb_vorbis` CPU is comfortably
  off the audio thread.

Audio thread: `process()` just `pull()`s a stereo frame (identical to Radio/Icecast Ninjam).

**v2 (later, optional):** expose per-user channels / per-user volume in realtime (à la JamTaba's
`NinjamController` + `NinjamTrackNode`) — pull per-user buffers and mix in `process()`. More
control, more complexity. Not needed to "hear the jam."

## 5. Protocol subset (verified; see TODO for full detail)

- **Framing:** `[type u8][size u32 LE][payload]`; LE ints, NUL-terminated UTF-8.
- **Auth (SHA1):** recv `AUTH_CHALLENGE 0x00` (challenge8, caps [keepalive secs in bits 8-15],
  proto 0x00020000, optional license) → send `AUTH_USER 0x80` with
  `response = SHA1( SHA1("user:pass") + challenge8 )`, anonymous = user `anonymous[:name]`,
  empty pass, caps bit1 set (+ bit0 to accept license) → recv `AUTH_REPLY 0x01` (flag bit0=ok).
  Then send `SET_CHANNEL_INFO 0x82` (even with zero channels) and `KEEPALIVE 0xfd` every caps interval.
- **Listen (recv):** `CONFIG_CHANGE 0x02` (BPM u16, BPI u16); `USERINFO_CHANGE 0x03`
  (per-rec: active, chidx, vol dB·10, pan, flags, user, chname); `DOWNLOAD_INTERVAL_BEGIN 0x04`
  (guid16, estsize, fourcc "OGGv", chidx, user; all-zero guid = silence);
  `DOWNLOAD_INTERVAL_WRITE 0x05` (guid16, flags [bit0=last], ogg bytes). Optional
  `SET_USERMASK 0x81` to subscribe (many servers auto-subscribe).
- **Transmit (send, later):** `UPLOAD_INTERVAL_BEGIN 0x83` / `UPLOAD_INTERVAL_WRITE 0x84`.

## 6. Phases & milestones

- **Phase 0 — scaffolding.** Vendor `stb_vorbis.h`; create `net/ninjam/` skeleton; SHA1 helper
  over OpenSSL; a standalone harness `test/njclient_test.cpp` (like `play_test`, links the
  ninjam layer + stb_vorbis + OpenSSL, no Rack).
- **Phase 1 — connect + auth.** TCP connect, challenge → SHA1 → reply, `SET_CHANNEL_INFO`,
  keepalive. **Milestone:** stay connected to a real server (ninbot/ninjamer) without being
  dropped; harness logs server caps. *Validate the auth bytes against a local canonical server.*
- **Phase 2 — metadata.** Parse `CONFIG_CHANGE` + `USERINFO_CHANGE`. **Milestone:** harness
  prints live tempo + roster matching the room's web view.
- **Phase 3 — decode the mix (the meat).** Interval reassembly by GUID, `stb_vorbis` decode,
  per-user resample + mix, interval-paced push to the ring. **Milestone:** hear the real
  multi-user jam in Rack; A/B against the Icecast mix of the same room. Harness verifies decoded
  PCM stats over several intervals.
- **Phase 4 — module UX.** "Join (protocol)" mode beside Icecast listen; pick room via
  `RoomDirectory` (host/port); panel shows tempo / roster / interval progress; clickable LED
  stop; persist mode + room. Keep Icecast as fallback.
- **Phase 5 — transmit (optional, big).** libvorbis/libogg encode; capture an input; align
  uploads to intervals; channel setup with our name. **Milestone:** others hear us in the jam.

## 7. Testing

- **Standalone harness** (no Rack/GUI) per phase — connect to a **private test server we already
  run** (`<redacted-host>`, port `<redacted-port>`; credentials in the agent's
  local memory, deliberately not in git) for offline, no-etiquette-risk testing, plus a real public
  server (ninbot/ninjamer) for an end-to-end check.
- **Byte layouts** cross-checked against JamTaba's serialization tests + wireshark captures.
- **A/B** the protocol mix against the existing Icecast mix of the same room.

## 8. Risks / gotchas

- Keepalive timing (server drops you if silent); send promptly after auth.
- Interval timing / sample slippage; the inherent 1-interval latency is expected, not a bug.
- No retransmit — validate the "OGGv" header per interval and skip corrupt ones.
- Resample remote rate → engine rate (we have a linear resampler; could upgrade to libsamplerate).
- Public-server etiquette: identify the client, honor the license-agreement bit, don't hammer connects.
- Decode CPU with many users — fine off-thread with an interval of lead time.

## 9. Decisions to confirm before building

1. **Scope:** listen-only first (recommended), or commit to transmit (Phase 5) too?
2. **Mixing model:** v1 pre-mixed stereo to the ring (simple) — OK? Per-user channels/volume is a
   later v2.
3. **SHA1 via OpenSSL** (reuse libRack) vs a tiny vendored SHA1 — recommend OpenSSL.
4. **UX:** keep Icecast listen as a fallback mode and present "Listen (stream)" vs "Join (protocol)"?

**Recommended first slice:** Phases 0–2 (vendor stb_vorbis + `NjProtocol` + `NjClient` connect/auth/
keepalive + metadata), proven by the standalone harness against a real server — small, verifiable,
and it de-risks the whole feature before the interval-decode work in Phase 3.
