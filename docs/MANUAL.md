# AK Audio — Manual

AK Audio is a collection of "network audio → Rack" modules by Andrei Kozlov. Every
connection is something *you* start; nothing is sent from your machine unless a module
explicitly transmits (only Ninjam's JOIN does). See the
[Privacy section of the README](../README.md#privacy) for details.

- [Radio](#radio) — streaming internet radio into a patch.
- [Ninjam](#ninjam) — listen to, or jam in, a NINJAM room.
- [Looper](#looper) — 8×8 interval looper on the jam's clock.
- [Recorder](#recorder) — archive the jam; export it as an Ableton Live set.

---

## Radio

A streaming internet-radio source. Point it at an Icecast/HTTP stream and it decodes the
audio and feeds it into your patch through a built-in level control.

### Panel

| Control | What it does |
|---|---|
| **LEVEL** knob | Built-in VCA. VCV AUDIO taper, from −∞ up to +12 dB. The ring around the knob shows the current gain. |
| **CV** input | Optional unipolar 0–10 V control over the level (scales the knob). |
| **▲ / ▼** stepper | Step through all stations (bundled + your own), deduplicated by URL. |
| **Station display** | Click to open the grouped station picker (thumbnails; the current station has a green ring). |
| **L / R** outputs | Stereo audio out. |
| **LED** | Lit only when audio is actually decoding and flowing — not merely "connected." |

### Playing a station

1. Click the station display to open the picker, or use ▲/▼ to cycle.
2. Pick a station; it connects and starts playing. The chosen station is saved with the
   patch and auto-resumes on load.

Stations are ordinary Rack **presets** — you'll also find them under right-click →
*Preset*, and under the *Stations* submenu in the context menu.

### Adding your own station

1. Right-click the module → paste a stream URL into the **Add station from URL** field.
2. The module *auditions* it off-thread: it confirms real audio is flowing (not just that
   the host answered), looks the URL up on radio-browser to get the real station name and
   icon, and caches the favicon locally.
   - **Identified and playing** → saved automatically under *Your stations*.
   - **Playing but unknown** → you're prompted for a name, then it's saved.
   - **Failed** → it rolls back to the previous station and shows why on the panel.

Nothing junk is ever saved, and saves are de-duplicated by URL against both bundled and
existing user stations. Direct stream URLs and `.pls`/`.m3u` playlists both work.

### Codec notes

- **MP3** works on all platforms.
- **AAC** and **HLS** (`.m3u8`) are macOS-only; on other platforms those streams report an
  error and MP3 stations keep working.

---

## Ninjam

A client for [NINJAM](https://www.cockos.com/ninjam/) online jam sessions. Two ways to
use it:

### LISTEN — hear a room, transmit nothing

Consumes a room's public Icecast/HTTP mix, exactly like Radio. No protocol handshake, no
login, and **your audio is never sent**. Good for just listening in.

### JOIN — play in the room

The full NINJAM protocol: connect to the server, anonymous login, decode the live
multi-user mix, **and transmit your own audio**. Audio on the module's **input jacks** is
encoded (downbeat-aligned OGG-Vorbis) and uploaded so everyone else in the room hears it.

> **JOIN is outbound.** Only use it when you intend to be heard. Chat messages you send
> also go to the server. LISTEN never transmits.

### Room browser

The panel includes a browser of public rooms (fetched from ninbot's directory):

- Search and scroll the list.
- Click a room to **listen**, or join it.
- A peak meter shows incoming level.

The interface never blocks on the network — the room list and connection run on
background threads.

### Panel

| Control | What it does |
|---|---|
| **IN** jacks | Your audio into the room (used on JOIN only). |
| **OUT** jacks | The mixed room audio out. |
| **Room browser** | Search / list / select public rooms; peak meter. |
| **Chat** | Send and receive room chat. |

Your last server, credentials, and room are saved with the patch.

---

## Looper

![Looper](images/Looper.png)

An 8×8 grid of one-interval loops — Ableton Session view for a NINJAM jam. Place it
directly next to a **Ninjam** module (either side) and it locks to the room's real
interval grid; without one it runs on a simulated clock (interval length in the context
menu). Every action is **boundary-quantized**: presses queue (blinking outline) and
commit exactly on the next downbeat. One queued action per track — the latest press
wins.

### Panel

| Control | What it does |
|---|---|
| **INS** (poly) | Your instruments: channels 1/2 = track 1 L/R, 3/4 = track 2, … Feed it from a mixer's direct-outs (a MindMeld MixMaster maps 1:1). |
| Per-track jacks | Alternative per-track stereo inputs (a track uses its own jacks when connected, else its INS channels). |
| **Track label** | Click to rename (4 characters, like MixMaster). With a MixMaster feeding INS, names sync both ways automatically — each side updates when an edit commits. |
| **Grid cells** | Empty: press to record the next interval (what you play *before* the downbeat folds into the take's tail — pickups survive). Filled: press to launch, press again while playing to stop. Recording: press to **finish** — the take commits on the downbeat and starts looping (a chain replays from its first cell); press again to keep recording instead. The waveform thumbnail fills live while recording. |
| **▶ scene** | Launch a whole row: filled cells play, empty cells stop that track. Latest scene press wins. |
| **■ / stop row** | Stop a track (or all); also disarms a rolling recording. |
| **OVERDUB** | Latch: the selected playing cell layers each interval until the latch is off. |
| **TX lamps** | Per-track on-air toggle: green = in the MIX (the room hears it), cyan = private — live input routes to CUE instead. |
| **OUTS / CUE / MIX** | Per-track poly out, private monitor out, and the stereo submix (cable MIX into Ninjam's IN to transmit it). |

### Playing through the downbeat

Keep playing past the loop point and the recording **auto-advances** into the next
empty cell, chaining downward until an interval comes in quiet — then the whole chain
is wired to replay in order from its first cell. Or end it yourself: **press the
recording cell** and on the next downbeat the chain closes and starts cycling from its
first cell (a final bar with nothing played in it is dropped, so the loop keeps its
meter). Pressing any *other* cell discards the rolling recording, as does ■.

### Tempo changes

A room BPM change re-pitches your takes tape-style (within 0.5×–2×; menu-toggleable);
a BPI doubling tiles takes ×2, halving splits them into chained halves. Derived takes
are RAM-only — disk always keeps the originals.

### Persistence

Every committed take is saved as a raw OGG under the jam folder
(`~/Music/jams/<date>/<time>…/looper/`), and the grid restores with the patch — takes
also embed in the `.vcv` itself (menu-toggleable), so a shared patch carries its loops.

---

## Recorder

![Recorder](images/Recorder.png)

The jam's black box. Place it directly next to a **Ninjam** module and arm **RECORD**:
every player's received intervals and (optionally, **REC TX**) your transmitted mix are
written to disk as the raw OGG bytes — no re-encode, nothing lost — under
`~/Music/jams/<date>/<time>_<room>/`, with a JSON-lines index on the shared session
timeline. The panel shows a live per-player interval count while recording.

### Ableton Live export

When recording stops, the Recorder automatically assembles the whole jam as an
**Ableton Live set** (`.als`) in the jam folder — openable directly in Live 11:

* the Looper grid as Session-view clips (looping, tempo-locked),
* your **as-played timeline**: which loop played when, reconstructed on each track's
  Arrangement lane,
* every remote player and your TX mix as Arrangement tracks,
* tempo, loop brace, and bar positions set to the jam.

Context menu: export any past jam folder on demand, toggle the auto-export, and pick
the **Target Live edition** — *Standard/Suite* (a track per player) or *Lite*
(fits the 8-track cap: 6 loop tracks, all players merged onto one lane — simultaneous
intervals get a proper audio mixdown — and your TX on the eighth).

Recording is deliberately gated on your explicit arm — nothing is written to disk, and
nothing of the room is captured, unless the Recorder sits armed next to a joined Ninjam.

---

## Building from source

See the [README](../README.md#building). In brief, with the Rack SDK (or a source build)
beside this repo:

```bash
make            # -> plugin.dylib / .so / .dll
make install    # package + install into the Rack user plugins folder
```
