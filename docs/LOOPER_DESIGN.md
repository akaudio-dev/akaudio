# Looper + Recorder — design document

Canonical design for two new modules of the akaudio VCV Rack plugin, both expanders
of **Ninjam**:

- **Looper** (left of Ninjam) — an 8×8 Ableton-Session-style interval looper for our
  instruments, submixed onto one NINJAM channel.
- **Recorder** (right of Ninjam) — a *wire archive*: saves every interval that went
  over the NINJAM connection, per player (and our own transmitted mix), as the raw
  OGG bytes, on a shared sample-accurate timeline — so a DAW project (`.als`/`.rpp`)
  can later reassemble the whole jam: our loops and everyone else's playing.

Decisions were reached in `docs/looper_plan.md` (the decision log). This document is
the consolidated statement, kept in sync with the code.

**Status (2026-08-22):** the **Looper is built and working through M1** — real interval
clock from Ninjam (M0), the Rack-free engine looping real audio (M1), the MIX + CUE
buses, and transmit-to-Ninjam over the expander. The **Recorder** module and its wire archive (§4 items 3-4, §7, M3) are also built.
Still on paper, marked *(planned)*: the Looper's **session files / disk persistence**
(§5.4 encode steps, §10, §11 — milestone M4), the timeline refinements in §7.3, and the
DAW project generators (§12). §13 tracks per-milestone status.

---

## 1. Goals and non-goals

**Goal.** Live jamming over NINJAM with the akaudio Ninjam module: loop 3-6 local
instruments with every action committed exactly on the interval boundary, and keep a
complete, DAW-importable record of the jam without any existing recorder.

**Must have (v1)**
- Looper: grid of **8 tracks × 8 slots**; a track = one stereo instrument (own jack
  pair, or a pair of channels from the single poly **MULTI** input); a slot = one take
  of exactly **one interval**.
- **Queued actions on the interval boundary** (capture / launch / stop / overdub /
  scene), with a sample-exact grid taken from Ninjam's protocol state — no CV
  plumbing, correct across mid-session BPM/BPI changes.
- **Ableton clip semantics, no record arm:** pressing an empty slot starts recording at
  the next boundary and the take starts playing at the boundary after that (replacing
  the slot that was playing). Every plugged-in track is continuously recorded into a
  rolling buffer, which is what makes the commit a pointer move.
- Per-slot **play modes** as two settings: `repeats` (∞ or N) and `decay` (gain per
  repetition).
- **Submix** of all tracks to one stereo MIX OUT → one NINJAM channel on the wire
  (a plain sum: instruments arrive already leveled and panned — no per-track
  level/pan on the panel).
- **Persistence = the grid:** ≤64 raw OGG files + `session.json`; overwritten takes
  are renamed into `history/`, never deleted.
- Waveform **thumbnails** in the slot buttons; an armed slot shows the interval being
  recorded filling in (like a recording clip in Live).
- Per-track **TX** latch (a bi-color LED, reusing Ninjam's transmit LED: green = on air
  → MIX, cyan = private → CUE). Off routes that instrument's live input to the **CUE**
  stereo out (your monitor) instead of MIX, crossfaded ~10 ms; loops still play to MIX,
  recording continues. For trying something without the room hearing it.
- **CUE** stereo out: the live-thru of the non-transmitting (private) tracks — monitor
  what you're working up before it goes on air.
- Everything pressable is a Rack param → MIDI-mappable (Launchpad-style).
- Recorder: per-player, per-interval `.ogg` files **as received** (+ our TX intervals
  as sent), an index with each interval's position on the **session timeline**,
  silence intervals as gaps, zero encoder CPU.

**Non-goals (v1)** — deferred, see §12: loading clips from disk (incl. restoring the
grid on patch reload), standalone clocking (no clock jack on the v1 panel),
multi-interval loops, follow actions, resampling takes, FLAC, the DAW project
generators themselves (`.als`/`.rpp` — v1 only guarantees the data they need), a
Recorder that decodes/mixes anything.

---

## 2. Topology

```
 instruments (8 stereo) ─► LOOPER ══ clock (expander) ══ NINJAM ── TX ─► room
                             │  MIX ══ audio (expander, no cable) ═► NINJAM transmit
                             ├─ MIX OUT jack ─► your monitor (on-air tracks + loops)
                             └─ CUE OUT jack ─► your monitor (private / TX-off tracks)
                                              NINJAM ◄── RX ◄── players
                                                │ raw interval bytes  (planned: Recorder)
                                                ▼
                                            RECORDER (planned) ─► session dir on disk
```

- **Looper** is an expander of Ninjam: it receives the interval grid through Rack's
  expander message buffers (Ninjam walks the chain on both sides, so more than one
  Looper may flank it, §3.3) and, in the other direction, writes its MIX into Ninjam's
  buffer so Ninjam transmits it with no cable (§6).
- **Recorder** needs no audio and no clock in `process()`: the byte archive
  (`NjArchive`) lives **inside Ninjam**, fed by `NjClient` on its network/TX threads;
  the Recorder module is its control panel + status, reached over the expander pointer
  on the UI thread. Ninjam archives only while a Recorder is adjacent.
- Effects belong **before** the Looper: a take then contains the sound the room heard.

---

## 3. Clock coupling (Looper)

### 3.1 Which clock

Ninjam has three interval clocks; the Looper follows the first:

1. **Ninjam's local beat clock** in `Ninjam::process()` — reset to beat 0 on join and
   on every server `CONFIG_CHANGE` (immediately); drives CLOCK/RESET/PHASE, the
   metronome and **`armTransmit()`** — i.e. the grid our uploads are cut on and the one
   the user hears as the downbeat.
2. `NjAudio::txLoop`'s integer-N cut (`N = llround(bpi·60·sr/bpm)`).
3. `NjAudio::mixLoop` playout — **arrival-locked per remote channel**, no global
   boundary. Not a clock to follow; it is what the Recorder timestamps (§7.3).

Clocks 1 and 2 drifted by a sub-sample fraction per interval (fractional `spb`
accumulation vs integer N); **`JamClock` (§4 item 1, built) makes clock 1 count integer
frames**, so RESET/PHASE, TX arming and the Looper now agree exactly.

### 3.2 `JamClockMessage` (`src/JamClock.hpp`, shared)

```cpp
namespace akaudio {
struct JamClockMessage {
	bool     running;          // JOIN mode with a tempo (bpm, bpi > 0)
	int      bpm, bpi;
	int      intervalFrames;   // integer N; 0 if !running
	int      frameInInterval;  // 0..N-1 — THE position
	int      beatIndex;        // 0..bpi-1 (display)
	bool     downbeat;         // frameInInterval == 0 this frame
	bool     beat;             // a beat boundary this frame
	uint32_t gridGeneration;   // ++ on every resync: join, tempo change, sr change
	uint64_t sessionFrame;     // monotonic frames since join — THE shared timeline (§7.3)
	float    sampleRate;
	char     roomLabel[64];    // NUL-terminated; for session naming
};
}
```

One struct per frame, ~100 bytes, copied by value.

### 3.3 Wiring

- **Looper allocates** `leftExpander.{producer,consumer}Message` and
  `rightExpander.{producer,consumer}Message` (`new JamClockMessage`, zeroed) in its
  ctor, frees them in its dtor — so it works on either side.
- **Ninjam writes** at the end of its `process()`, per side:
  ```cpp
  Module* m = rightExpander.module; int hops = 0;
  while (m && m->model == modelLooper && hops++ < MAX_LOOPER_HOPS /*8*/) {
      auto* buf = (JamClockMessage*) m->leftExpander.producerMessage; // our side of it
      if (!buf) break;
      *buf = msg;
      m->leftExpander.requestMessageFlip();
      m = m->rightExpander.module;
  }
  ```
  (mirror on the left using `m->rightExpander`). Rack flips every module's messages at
  the end of the same timestep, so **all Loopers in a chain see the clock with a
  uniform 1-sample latency**; Loopers are pure readers. Expander pointers are updated
  by the engine between blocks under its lock — chasing them from `process()` is safe.
  Any non-Looper module (incl. the Recorder) ends the chain.
- **Looper reads** whichever side's `consumerMessage` is live (`sessionFrame`
  advancing); prefer left if both. A message that stops advancing ⇒ source lost
  (§9.4).
- 1-sample latency vs Ninjam's own TX cut is constant and deterministic — documented,
  not compensated.

### 3.4 `IntervalSource`

The engine consumes an `IntervalSource` yielding `JamClockMessage`-shaped frame
state. v1 implements only the expander reader; no Ninjam ⇒ idle (§9.3 covers losing
it mid-jam). A standalone source is deferred **without reserving a jack** (decided
2026-08-22): its shape isn't settled — intervals may become per-track or per-take —
and adding a jack later is patch-safe, whereas a global RESET jack now would presume
one shared boundary. The engine keeps **N per track** from day one.

---

## 4. Ninjam changes (complete list)

No protocol, socket, or codec changes.

0. **Expander TX audio** (`Ninjam.cpp`): Ninjam allocates `LooperAudioMessage` receive
   buffers on both sides, reads an adjacent Looper's MIX from them each frame, and — when
   a Looper is its neighbour — transmits that one stereo channel (via `captureFrame(0,…)`)
   and declares one channel, instead of the poly IN jack. IN jack unchanged when no
   Looper is adjacent. The Looper writes the buffer + requests the flip.
1. **`JamClock` extraction** (`Ninjam.cpp`): the beat clock becomes a small struct
   counting **integer frames** (`frameInInterval++`, boundary at `N`, `beatIndex =
   frameInInterval·bpi / N`; `N` exactly as `NjAudio::recomputeIntervalLocked`), plus
   the monotonic `sessionFrame`. `tick()` yields the `JamClockMessage`; Ninjam uses it
   for its own jacks, metronome and TX arming, then writes it to the expanders.
   `gridGeneration++` on the existing `resyncBeat` branch and on sample-rate change.
   Behaviour change for existing users: sub-sample.
2. **Expander chain walk** on both sides (§3.3).
3. **`NjArchive`** *(built, `src/net/ninjam/NjArchive.{hpp,cpp}`)* (`src/net/ninjam/NjArchive.{hpp,cpp}`, Rack-free) —
   the wire archive, owned by Ninjam, with its own writer thread and a bounded job queue:
   - **RX:** `NjClient` gets an optional callback `onIntervalReceived(user, chidx,
     guid, bytes, mixFrameStart)` fired by `NjAudio` when the mix thread *starts
     playing* an interval (so the timestamp is the local playout start, §7.3); a
     zero-guid silence interval fires with empty bytes (a gap in the index).
   - **TX:** `NjClient::sendUploadBegin/Data` also hand each outgoing interval's bytes
     (+ its `sessionFrame` start, known from the arming point and N) to the archive
     once the final chunk is sent.
   - **Timeline mapping:** the audio thread publishes `pullOffset = sessionFrame −
     framesPulled` (atomic, updated when it changes — i.e. at start and across
     underruns); the archive converts an RX interval's `mixFrameStart` to
     `sessionFrame = mixFrameStart + pullOffset`. Exact except across an underrun,
     which is itself an audible glitch.
   - Writes are atomic (tmp + rename); the index is appended per interval.
   - `start(dir, opts)` / `stop()` / `status()` (mutex-guarded snapshot: per player
     name, intervals, bytes, last activity) are UI-thread API used by the Recorder.
4. **Archive gating** *(built)***:** Ninjam's widget `step()` checks whether a Recorder is adjacent
   on either side and starts/stops the archive accordingly (UI thread — module
   removal also happens there, so no stale pointers).

Keep in mind: `recomputeIntervalLocked` rejects intervals > 1<<22 frames (≈87 s at
48 kHz). 64 BPI at 60 BPM is fine; 128 BPI at 60 BPM is not — raise the cap if needed.

---

## 5. Looper engine

Code layout: `src/looper/` is **Rack-free** (testable from `test/`): `LooperEngine`
(audio-thread state machine + mixing), `LooperWorker` (thread + jobs), `Session`
(files + JSON). `src/Looper.cpp` is the Rack `Module` + `ModuleWidget` glue.
`src/JamClock.hpp` is shared with Ninjam.

### 5.1 Data

```
Take      { Buf* buf (N×2 interleaved, immutable once committed); int frames;
            int bpm, bpi; float sampleRate; uint64_t startFrame /*session timeline*/; float peak; }
Slot      { Take take; int repeats /*0 = ∞*/; float decayDb /*0…−6 dB per repetition*/;
            State state; Pending pending; int repCount; float gain; bool playable;
            float thumb[THUMB_BINS]; Buf* staging /*overdub*/; }
Track     { Buf* rec, *last, *spare (rolling: recording / just-completed / pre-fetched);
            bool present; float txGain, gateEnv;
            Slot slots[8]; int playingSlot; float live[THUMB_BINS] /*armed slot draws it*/; }
```

- **Buffers are N-frame stereo 32f.** 16 s (120 BPM·32 BPI) – 32 s (60 BPM·32 BPI)
  @48 kHz = 6–12 MB each. We jam at 32 BPI.
- **Accounting:** up to 3 rolling per track (`rec` + `last`/`spare`) + 1 per filled
  slot (on demand) + 1 staging per in-flight overdub. Rolling buffers **follow cable
  presence** — an unpatched track holds none. Fully populated 8×8 at 32 BPI ≈ 0.4–0.8 GB.
  Track names/labels live in the Rack module (`src/Looper.cpp`), not the engine.
- **A committed take is immutable.** Playback and the worker's encode/thumbnail read
  it concurrently; every mutation (overdub) produces a new buffer and swaps it in.
- **Free-list** on the worker: same-size buffers within a grid generation; flushed when
  `gridGeneration` changes N.

### 5.2 Slot states and pending operations

```
state:   Empty | Filled | Playing | Recording
pending: None | Capture | Launch | Stop | Overdub     (committed at the boundary)
```

Button semantics (edges detected in `process()`; "armed" = pending set, light blinks;
pressing again cancels):

| gesture | Empty | Filled | Playing | Recording |
|---|---|---|---|---|
| slot press | arm **Capture** (→ Recording at the boundary → Playing at the next) | arm **Launch** | arm **Stop** (or **Overdub** if the OVERDUB latch is on) | **cancel** (→ Empty) |
| scene press (row) | track **stops** (empty slot in the row) | arm Launch | no-op if already the playing slot | leave it |
| track STOP / STOP ALL | — | — | arm Stop | — |

**Any** slot press also **selects** that slot for the REPEATS/DECAY knobs (a ring shows
it); there is no separate select gesture. One playing slot per track: arming Launch
elsewhere replaces it at the boundary. Scenes use Ableton's default semantics (a scene
is a complete state of the band).

**Per-clip settings (`repeats`, `decay`) — how they are changed:**
- **Selection = the last slot you pressed** (arm + select together), shown as a ring.
  MIDI pads select as a side effect of launching.
- The **REPEATS** and **DECAY** knobs (bottom strip; real params ⇒ MIDI-mappable)
  show and edit the selected slot's settings: turning a knob writes to the selected
  slot; changing the selection reloads the knobs (a `ParamStateSync`-style reconcile
  in the widget's `step()`). REPEATS snaps to {∞, 1, 2, 4, 8, 16, 32, 64}; DECAY is
  0 … −6 dB **per repetition** (gain = 10^(dB/20) per wrap).
- The **slot's right-click menu** (Rack's param context menu, extended) offers the same
  two settings as submenus plus *Select* and *Clear slot* — editing there never arms.
- **The slot shows its settings**: a corner tag (`∞`, `×4`; `↘` when decay < 0 dB);
  a playing finite slot counts down (`3 left`).
- **Defaults for new captures** are module-level (context menu: *New clips: repeats /
  decay*; default ∞ / 0 dB). New captures do not inherit the knob positions.

**At the boundary** (per track, in order):
1. Finish the rolling interval: swap `rec[0]`↔`rec[1]`; compute its peak; reset the
   live thumb; note its `startFrame` (= `sessionFrame` at the boundary that began it).
2. Commit pending ops whose worker preparation has arrived (§5.4); others wait one
   more interval.
   - **Capture** (Ableton clip semantics, two boundaries): a slot armed Capture enters
     **Recording** at this boundary and records the interval that now begins (refused →
     Empty only if the track has no input/buffer). At the **next** boundary the
     just-completed rolling buffer *becomes* `slot.take` (pointer rotation, no copy) and
     the slot → Playing, replacing the previously playing slot; a **silent** recording
     (peak < gate) is refused → Empty. Only one Recording slot per track (arming another
     cancels it).
   - **Launch**: Playing; the previously playing slot → Filled.
   - **Stop**: Filled.
   - **Overdub**: staging (take + this interval's input, folded in progressively by
     `process()`) swaps in as the new take.
   *(The `Committed`/`Overwritten` encode-and-write steps are M4; the M1 engine keeps
   takes in RAM only.)*
3. Playing slots wrap: `repCount++`; `gain = decay^repCount`; stop if `repeats &&
   repCount == repeats` or `gain < 1e-3` (−60 dB). Repeat/decay edits apply at the wrap.

### 5.3 Per-frame `process()` (audio thread)

```
msg = clock.tick()                                    // live Ninjam message, else simulated (§9.3)
if msg.gridGeneration changed → onRegrid()            // §9.1
for each track t:
    in  = input(t)   (own jack pair, else its MULTI pair; R := L if R unconnected; §8.2)
    if msg.downbeat → §5.2 commit sequence
    rec[frame] = in; live[frame·BINS/N] = max(…, |in|)          // always-record (armed slot draws it)
    if slot overdubbing → staging[frame] += in
    loop  = playingSlot ? take[pos]·gain : 0
    thru  = gate(in)                                  // −70 dBFS, 100 ms hold → exact 0
    mix  += thru·txGain + loop                        // on-air share + loops (loops always to MIX)
    cue  += thru·(1 − txGain)                         // private share (TX-off), crossfaded on txGain
MIX OUT = limiter(mix) ;  CUE OUT = limiter(cue)      // safety limiter (transparent to full scale)
```

The repo's realtime contract applies in full: no allocation, no locks, no I/O, no
logging. Cross-thread traffic = two SPSC queues (§5.4) + atomics for the UI.

### 5.4 Worker thread (one per Looper; started in the Module ctor, joined in the dtor)

Commands from `process()` (SPSC, fixed-size POD; the worker is the **only** thread that
allocates/frees):
- `ALLOC{track, frames, seq}` → allocate a zeroed N-frame buffer (Capture into an empty
  track's rolling spare; keeping the rolling pair stocked) → reply `ALLOC{buffer}`.
- `OVERDUB_COPY{track, slot, seq, take, rec, upto}` → staging = take + `rec[0..upto)`
  (the part already recorded when the press landed); the audio thread folds in the rest
  as it records → reply `OVERDUB_COPY{buffer}`.
- `RELEASE{buffer}` → recycle onto a small same-N free-list (flushed when N changes), so
  arming never waits on `malloc`.

*(planned, M4)* the take's OGG encode + `session.json`/`history/` writes become worker
jobs (`Committed`/`Overwritten`/`Cleared`), serialized per slot. The M1 worker does
buffers only — no encoding or file I/O yet.

### 5.5 UI thread

Reads atomics only: per-slot state/pending/repeats/decay/gain/repCount/playable, the
module's interval `phase` (0..1) for the playhead, per-track `present`, and the `live` /
`thumb` float arrays (unsynchronized reads — benign display races; written only by the
audio thread at frame or boundary granularity).

---

## 6. Submix and gating

- MIX OUT = Σ (on-air thru + loop); **CUE OUT = Σ (private thru)**, the live input of
  TX-off tracks (loops always stay in MIX). No per-track level/pan: instruments are assumed to arrive
  already leveled and panned at line level (±5V), so the sum is unity — a single track
  in = the same level out. A soft limiter is a **safety net only**: transparent up to
  full scale (±5V), gently compressing a SUM that exceeds it (a loop stack) so it can't
  hard-clip. It is not a level control. What you monitor = what the room hears.
- Live-thru is **gated to exact zero** (−70 dBFS, 100 ms hold): measured with the
  vendored encoder, exact zeros cost ≈0.1 kbps but noise at −90 dBFS ≈130 kbps, so
  idle tracks must be true silence on the wire.
- **One** stereo channel on the wire (`NjAudio::MAX_TX = 4` is not a constraint).
  Trade-off: collaborators get one fader for "you", not one per instrument.
- **No MIX→IN cable needed.** A Looper adjacent to Ninjam hands its MIX to Ninjam over
  the expander (a `LooperAudioMessage` in `JamClock.hpp`: the Looper writes its on-air
  MIX into Ninjam's expander buffer each frame; Ninjam OWNS that buffer as receiver and
  reads it, 1-sample latency like the clock). Ninjam then transmits that single stereo
  channel instead of its poly IN jack. Precedence: an adjacent Looper wins; with no
  Looper neighbour Ninjam's IN jack behaves exactly as before (standalone use intact).
  Left neighbour preferred (the instruments looper sits on Ninjam's left). The MIX OUT
  jack still exists — you cable it to your monitor; only the MIX→Ninjam-IN cable is gone.
  (Decided 2026-08-22, reversing the earlier "audio stays on cables" non-goal — the two
  are already an adjacent pair for the clock, so the cable was redundant.)
- Optional Ninjam follow-up: emit a NINJAM **silence interval** (zero GUID) for an
  all-zero captured interval instead of encoding it.

---

## 7. Recorder

*(§7 is **built** — milestone M3 — with two simplifications from the sketch below,
noted inline: the RX timestamp is a coarse session frame Ninjam publishes each block
(not the exact playout-start mapping of §7.3, which is a v2 refinement), and the index
is `index.jsonl` — one JSON object per line — rather than `index.json` + `clipsort.log`.)*

### 7.1 What it is
A Ninjam expander with **no audio path and no clock reading** (`src/Recorder.cpp`, 8 HP):
REC arm + "+TX" toggle + a live per-player status list. It reaches Ninjam through the
`RecorderLink` interface (`src/RecorderLink.hpp`) by `dynamic_cast` on its adjacent
module — no expander messages, no cables. The archive itself lives in Ninjam
(`NjClient` owns `NjArchive`; RX bytes via `NjAudio::onIntervalReceived`, TX bytes
accumulated in `sendUploadData`). Value over any existing recorder: **exact per-player,
per-interval OGG files** (the bytes each player actually sent, lossless w.r.t. the
wire), **names** from the username, **our own TX** intervals as sent, on a shared
timeline with the Looper — at zero encoder CPU. Privacy: the archive only runs while a
Recorder is physically adjacent AND armed AND Ninjam is joined.

### 7.2 Files
```
<asset::user("akaudio-sessions")>/<YYYY-MM-DD_HHMM>_<room>/players_tx/
    index.jsonl                     one JSON object per interval (append-only)
    players/<seq>_<user>_ch<n>.ogg  one file per received interval (silence = no file)
    tx/<seq>_mix.ogg                our transmitted intervals, as sent (if "+TX" on)
```
Each `index.jsonl` line: `{seq, tx, user, chidx, file, sessionFrame, bytes, bpm, bpi,
frames, sampleRate}`. `seq` is a global monotonic interval counter; files are written
atomically (tmp + rename). Disk: at the senders' quality, ~1 MB/min per busy player,
nothing for silence. *(v2: also emit a NINJAM `clipsort.log` for REAPER's native
session import.)*

### 7.3 The shared timeline
`sessionFrame` (monotonic frames since join, in the clock message) is the axis for
everything: the Looper stamps each take's `startFrame`; `NjArchive` stamps each
received interval's local playout start (`mixFrameStart + pullOffset`) and each TX
interval's start. Received intervals are arrival-locked (arbitrary 0–1 interval
offset from our grid) — the timeline records that offset exactly, so a DAW project
reproduces the jam *as heard here*. Tempo changes don't reset the axis; each entry
carries its own `(bpm, bpi, frames)`.

### 7.4 Panel (≈8 HP)
REC button (arm/disarm; LED), session folder name, "record own TX" toggle, and a
list of players: name, activity LED, interval count, MB written, "(left)" for
departed users. Context menu: open session folder, new session. Status comes from
`NjArchive::status()` over the expander pointer on the UI thread; if no Ninjam is
adjacent the panel says so.

---

## 8. Looper panel

Ableton Session layout, **44 HP** (68 px columns; I/O in a narrow controls column right of the grid), house style as
Radio/Ninjam (`ebebeb→e1e1e1`, `#1f1f1f` Nunito-Bold title).

```
 ┌──────────────────────────────────────────────────────┬────────┐
 │ LOOPER            NINJAM · 120 BPM · 32 BPI · next 12s ●│        │  header: source line + sync LED
 ├──────┬──────┬─ … ─┬──────┼────────┤  L,R jacks SIDE BY SIDE per track
 │(L)(R)│(L)(R)│     │(L)(R)│ [MULTI]│  poly MULTI jack sits in the controls column
 │[-01J]│[bassP]│    │[-08-]│  ◎ DUB │  editable label + source tag (J / P3-4, green=live)
 ├──────┼──────┼─ … ─┼──────┤ REPEATS│
 │clip  │ clip │     │ clip │  ◯     │  8×8 clip grid            scene ▶ column
 │ grid │ grid │ …8× │ grid │ DECAY  │  (right edge, not shown)
 │  …   │  …   │     │  …   │  ◯     │
 ├──────┼──────┼─ … ─┼──────┤ ┌────┐ │
 │[■ ◉] │[■ ◉] │     │[■ ◉] │ │CUE │ │  track STOP + bi-color TX LED (green=MIX, cyan=CUE)
 ├──────┴──────┴─ … ─┴──────┤ │L  R│ │  STOP ALL under the grid
 │  scene ▶ column at right  │ ├────┤ │  ┌────┐
 │                          │ │MIX │ │  │ AK │  output plates: CUE over MIX
 └──────────────────────────┴─┴L  R┘─┴──┴────┘
```

### 8.1 Components
- **Slot buttons:** rectangular (~11 × 7 mm), custom widget drawing the thumbnail.
  Fill = state: Empty (flat) · Filled (grey waveform) · Playing (accent waveform +
  playhead line) · armed (blinking outline) · selected (white ring) · refused capture
  (red flash).
- **An armed slot shows the interval being recorded** filling left→right (red for
  capture, amber over the take for overdub) — the "what will I capture" preview lives
  in the slot, not in a separate strip. `process()` accumulates the per-bin peak into
  the track's live thumb (one compare per frame); the armed slot's widget reads it.
- **Track labels**: MindMeld-style dark boxes with amber monospace text, default
  `-01-`…`-08-`; click opens an inline editor; persisted in the patch and written to
  `session.json`.
- **Scene** ×8 (▶ glyph, no number), **track STOP** ×8 (full-column-height raised
  buttons, visually distinct from the sunken clip cells) each with a **bi-color TX LED**
  beside it (`akDrawTxLedC` in Theme.hpp: green = on air → MIX, cyan = private → CUE),
  **STOP ALL**, **OVERDUB** latch: all params (MIDI-mappable). No per-track
  mute/level/pan — mute at the mixer, levels arrive set.
- Controls column right of the scenes, top → bottom: **OVERDUB** (component-library
  bezel button + red light, as Fundamental's PUSH), REPEATS, DECAY, the poly **MULTI**
  input, then **CUE** and **MIX** on output **plates** (L over R; Theme `AK_PLATE_*`
  style; MIX aligned to Ninjam's two output rows). No per-track POLY out — Ninjam's
  poly players out covers that. The **AK** mark at the shared `AK_MARK_Y_MM`. No
  "SCENES" label; no RESET jack.
- Header: source line ("NINJAM · bpm · bpi · next in Ns" or "SIMULATED CLOCK …") + a
  sync LED (green: locked to a Ninjam clock; dim: simulated/idle) and a progress bar.

### 8.2 Inputs
Two ways in, per track: its own **stereo jack pair**, or the single poly **MULTI**
jack (in the controls column, where one cable from a mixer replaces 16): MULTI channels
are **fixed stereo pairs** — 1-2 → track 1, 3-4 → track 2, … (an odd trailing channel
is mono). **A track's own L jack takes precedence** over its MULTI pair; R unconnected
⇒ R = L. The mapping never moves: adding a jack to a track doesn't shift any other
track's channels. `present(t)` = own L connected, or MULTI carries channel 2t. A track
is recorded whenever present; a take is kept only if it clears the gate. Each track
label carries a source tag (J = own jack, P3-4 = MULTI wires, green when audio is
actually arriving).

---

## 9. Edge cases (Looper)

### 9.1 Grid change (`gridGeneration`)
Cancel all pending ops; restart the rolling recorders at the new frame 0. If N is
unchanged (re-join at the same tempo): playing slots keep playing, playheads restart
at the new downbeat. If N changed: playing slots → Filled; takes whose `(frames,
sampleRate)` ≠ live grid are **greyed** (not launchable) until it matches again.
Free-list flushed.

### 9.2 Sample-rate change — arrives as a grid change. Old-rate takes greyed;
resampling on load is v2.

### 9.3 Clock source lost (Ninjam removed, left the room, LISTEN mode)
The Looper falls back to its **simulated clock** (the context-menu interval), so it
keeps running and the UX still works with no Ninjam in the rack. Because that is a
source switch, it counts as a grid change (§9.1): pending ops cancel and takes whose
length ≠ the simulated N are greyed — so playing loops of a different length stop.
**Known gap vs. the intent** ("losing the connection must not kill the music"): keeping
loops running at the *last Ninjam N* across the drop (instead of jumping to the
simulated grid) is a v2 refinement.

### 9.4 Silent capture — refused (red flash, slot stays Empty); no empty files.

---

## 10. Session files  *(planned, M4 — not built; the M1 engine keeps takes in RAM only)*

```
<asset::user("akaudio-sessions")>/<YYYY-MM-DD_HHMM>_<room>/      one jam
    looper/
        session.json
        t<track>_s<slot>.ogg                     ≤ 64 live files (raw takes, archive quality)
        history/<YYYYMMDD-HHMMSS>_t<track>_s<slot>.ogg   overwritten/cleared takes (renamed, never deleted)
    players/ , tx/                               the Recorder's archive (§7.2)
```
The Looper and the Recorder agree on the session directory through Ninjam: the
current session name is Ninjam's (room + join time), exposed in the clock message's
`roomLabel` and the archive status, so both land in the same folder.

`looper/session.json`:
```json
{ "version": 1, "created": "…", "room": "…",
  "bpm": 120, "bpi": 32, "sampleRate": 48000, "intervalFrames": 768000,
  "tracks": [ { "index": 0, "name": "piano" } ],
  "slots":  [ { "track": 0, "slot": 0, "file": "t0_s0.ogg", "repeats": 0, "decay": 1.0,
                "created": "…", "startFrame": 6144000, "frames": 768000,
                "bpm": 120, "bpi": 32, "sampleRate": 48000, "peak": 0.71 } ] }
```
- Takes are stored **raw**; repeats/decay are non-destructive metadata.
- Encoding: `encodeOggInterval()` at **archive quality q≈0.8** (~250 kbps, ~1 MB per
  16 s take); measured 0.2–0.5 % of one core per stream, so 9 encoders (8 + Ninjam's
  TX) ≈ 5 % of a core worst case. Writes are atomic (tmp + rename).
- Overdubs re-encode from the 32f RAM copy: first-generation within a session.

---

## 11. Patch persistence (`dataToJson`)

*(Built today: `simSecondsIdx`, `defRepeats`, `defDecayDb`, `trackNames`, and the Rack
params — no audio, so a reload gives an empty grid. The session-directory / loader part
below is **planned, M4**.)*

Looper: session directory (relative to `asset::user("akaudio-sessions")` — never an
absolute path that embeds the home directory), archive quality, track names,
per-slot `repeats`/`decay`, selected slot; params persist themselves. **No audio.**
In v1 a reload gives an **empty grid**; files remain on disk and the persisted path
lets the v2 loader restore them. Recorder: "record own TX" toggle and armed state
(re-arms on patch load only if Ninjam auto-rejoins — it records nothing otherwise).

---

## 12. Deferred (v2+)

**DAW project generators** — the payoff of the timeline: REAPER `.rpp` (plain text,
first) and Ableton `.als` (gzipped XML, reverse-engineered; Live imports Ogg Vorbis):
8 Looper tracks with clips in Session View *and* an Arrangement of every player's
intervals at their `sessionFrame`. Clip loader (restore grid on patch load; load from
files). Standalone clocking with per-track/per-take interval lengths. Duplicate /
extend-with-silence (multi-interval loops). Follow actions; per-slot "no stop"; tape-
style degradation. History browse. FLAC slot files. NINJAM silence-interval TX.
Resample takes on sample-rate change. Recorder: decode-on-demand preview per player.

---

## 13. Milestones and tests

| # | Milestone | Proves |
|---|---|---|
| UX | **Do-nothing scaffold**: full Looper panel (grid, scenes, stops, editable labels, REPEATS/DECAY, overdub ring, jacks), a *simulated* interval clock + the real slot state machine (arm/commit/scene/repeats/decay/selection/menus), real live fill (armed slot) and capture thumbnails from the inputs, input pass-through — but no audio stored or played | the UX, before any engine or Ninjam change |
| M0 | **Real clock.** `src/JamClock.hpp` (`JamClockMessage` + a `JamClock` that counts integer frames: `intervalFrames`, `frameInInterval`, `gridGeneration`, `sessionFrame`); Ninjam's beat clock extracted onto it (CLOCK/RESET/RUN/PHASE, metronome and TX arming unchanged in behaviour, sub-sample drift fixed) + the expander chain walk on both sides; the Looper allocates its message buffers, detects a live Ninjam clock and drives its boundary/countdown from it (the simulated clock stays as the no-Ninjam fallback for now). *(The scaffold part of the old M0 — slug, panel, `plugin.json` — was done in UX.)* | the sync path: a Looper next to a joined Ninjam commits on the room's downbeat |
|    | *Status 2026-08-22: implemented* — `src/JamClock.hpp` + `test/jamclock_test.cpp` (passes); Ninjam's beat clock runs on `JamClock` and calls `publishClock()` every frame; the Looper reads both sides, takes a live message (running + `sessionFrame` advancing) over the simulated clock, cancels queued actions on a `gridGeneration` change, and shows "NINJAM · bpm · bpi" with a green sync LED. Behaviour change in Ninjam: the metronome click and TX arming now happen *on* the regrid downbeat (join / tempo change) instead of one beat later. `roomLabel` is still empty (needs an audio-thread-safe snapshot of the UI-owned label). | |
| M1 | `LooperEngine` (Rack-free): rolling record, Capture, Launch, Stop, playback with repeats/decay; single track; worker with Prepare/Ready and the free-list | the state machine + buffer rotation, under test |
|    | *Status 2026-08-22: implemented* — `src/looper/{Spsc,LooperEngine,LooperWorker}` + `test/looper_engine_test.cpp` (passes, sample-exact). Beyond the M1 scope it already covers all 8 tracks, scenes, overdub (progressive staging: worker copies take + the recorded part, audio thread folds the rest in ≤256 frames/tick), the TX latch, the −70 dBFS gate and a tanh soft limiter; rolling buffers follow cable presence (an unpatched track holds none). The module is wired to it: the Looper **loops real audio** in Rack from an adjacent Ninjam's clock or the simulated one. Capture rotates `last` into the slot with no copy and never waits on the worker; a second capture of the same interval on one track is refused. | |
| M2 | 8×8 grid, scenes, track STOP / STOP ALL, submix + gate + limiter, OVERDUB | the full Looper |
|    | *Status 2026-08-22: done, folded into M1* — plus CUE bus + bi-color TX LED, MULTI input with source tags, and transmit-to-Ninjam over the expander (§6). | |
| M3 | `NjArchive` + `NjClient` RX/TX callbacks; Recorder module (panel, arm, status) | the wire archive and the shared timeline |
|    | *Status 2026-08-22: implemented* — `NjArchive` (Rack-free writer thread) + `test/archive_test.cpp` (passes); RX bytes via `NjAudio::onIntervalReceived`, TX accumulated in `NjClient::sendUploadData`; `src/Recorder.cpp` drives it through `RecorderLink` (dynamic_cast on the neighbour); Ninjam gates on adjacency+arm+join and publishes the session frame. Simplifications vs §7.3: coarse per-block RX timestamp; `index.jsonl` not `clipsort.log`. | |
| M4 | Looper `Session`: OGG writes, `session.json`, `history/`, patch persistence; one session dir shared with the Recorder | nothing kept is lost |
| M5 | Looper panel: thumbnail slot widget with live fill, header, context menu (quality, memory) | the UI |
| M6 | Docs (MANUAL.md, CHANGELOG), Library release | ship |

Tests (`test/`, no Rack link):
- `jamclock_test.cpp` *(built, passes)* — the integer `JamClock`: N vs NjAudio's
  formula, one downbeat + `bpi` beats per interval, timeline across a tempo change,
  fresh session on rejoin.
- `looper_engine_test.cpp` *(built, passes)* — drives `LooperEngine` with a synthetic
  clock + deterministic input: record-next-interval capture and launch/stop/overdub
  play back sample-exactly; a new recording plays the old loop until it commits; repeats;
  −6 dB decay; refused silent recording; scene stop; clear; regrid greys + refuses a
  mismatched take; CUE carries a private track's thru (not MIX); allocation count bounded.
- `session_test.cpp` — write a Looper session, parse it back, overwrite a slot and
  find the old file in `history/`, atomicity (no partial files after a simulated abort).
- `archive_test.cpp` *(built, passes)* — feed `NjArchive` synthetic RX/TX intervals
  (incl. a silence interval that writes no file) with changing session frames; assert
  the per-player/tx files, verbatim bytes, JSONL index entries, and stats. `make unittest`
  builds + runs jamclock/looper_engine/archive together.
- Existing `enc_test.cpp`: add the silence-size assertion (≲ 8 KB per 20 s).
- Manual: Looper + Ninjam + Recorder on a real room; REAPER import of `clipsort.log`.
