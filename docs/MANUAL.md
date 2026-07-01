# AK Audio — Manual

AK Audio is a collection of "network audio → Rack" modules by Andrei Kozlov. Every
connection is something *you* start; nothing is sent from your machine unless a module
explicitly transmits (only Ninjam's JOIN does). See the
[Privacy section of the README](../README.md#privacy) for details.

- [Radio](#radio) — streaming internet radio into a patch.
- [Ninjam](#ninjam) — listen to, or jam in, a NINJAM room.

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

## Building from source

See the [README](../README.md#building). In brief, with the Rack SDK (or a source build)
beside this repo:

```bash
make            # -> plugin.dylib / .so / .dll
make install    # package + install into the Rack user plugins folder
```
