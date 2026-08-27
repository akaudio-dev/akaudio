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

**Status (2026-08-23):** the **Looper is built and working through M4** — real interval
clock from Ninjam (M0), the Rack-free engine looping real audio (M1), the MIX + CUE
buses, transmit-to-Ninjam over the expander, and **session files on disk** (M4): each
committed take is encoded to OGG + indexed under `<base>/<stamp>_<room>/looper/`,
overwritten/cleared takes retired into `history/`. The **Recorder** module and its wire
archive (§4 items 3-4, §7, M3) are also built, and the **clip loader** restores the grid
on patch reload (§11 — the saved OGGs decode back into their slots). Still on paper, marked
*(planned)*: the timeline refinements in §7.3. The **Ableton `.als` exporter** (§12) is
built (2026-08-25, attempt #2 — template-based); REAPER `.rpp` stays open. §13 tracks
per-milestone status.

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

**Any** slot press also **selects** that slot (a ring shows it — the selection is the
continuous overdub's target); there is no separate select gesture. One playing slot per track: arming Launch
elsewhere replaces it at the boundary. **Recording takes over the track**: when a
Capture commits (→ Recording), the track's playing clip stops on that same boundary —
the old loop is never audible under the instrument being recorded (a refused silent
capture therefore leaves the track stopped). **Pickup capture (press → downbeat)**: the
always-on rolling record means the audio performed between *pressing* the cell and the
downbeat is still in the buffer that rotates back into `rec` at the commit boundary —
it is folded into the committed take's **tail** (a loop is circular: the pickup replays
right before each repeat's downbeat, exactly as performed). An early-hit attack a few
ms before the beat and a full lead-in phrase both survive; pre-press noodling never
does. The fold is faded in over ~1.5 ms, updates the take's peak + thumbnail tail
bins, runs *after* the auto-advance tail-gate scan (a folded lead-in must not read as
"played through the downbeat") and *before* the disk save (the OGG carries it).
Chained auto-advance cells skip the fold — their pickup is the previous cell's tail,
contiguous on replay by construction. **Auto-advance capture** (on by default;
context-menu toggle *Capture: auto-advance while playing*): if the committed take's
final ~300 ms are hot (≥ −40 dB — the player blew through the downbeat), the recording
rolls into the **next empty slot below** instead of the take starting to loop; one
interval per cell, silent while it rolls (recording still owns the track). **The chain
wires itself as it commits**: when a cell's successor really lands, the cell gets
`repeats 1` + `follow → next` — so launching the performance's first cell replays the
whole take in order, once (the last cell's follow stays *Stop*; wiring waits for the
successor's commit, so a follow never dangles on a refused cell). The chain ends when
an interval is fully silent (the −70 dB gate refuses it — cells sit FILLED, wired,
nothing auto-plays), when a chain member ends with a quiet tail (that outro cell
commits FILLED + `repeats 1` instead of looping alone), when the next slot is occupied,
or at the bottom of the column (those two commit-and-play as usual). **The OVERDUB
latch suppresses the chain**: playing through the downbeat then layers onto the
committed cell (the continuous overdub arms on it) instead of rolling into the next
one. A quiet tail on a
standalone capture = the player stopped before the loop point: the take loops
immediately (the classic one-interval capture). Cancelling the armed cell (a press), clearing either chain member, a clip-loader
install landing on one, or capturing elsewhere ends the chain the same way
(`breakChain`: predecessor stamped `repeats 1` when its armed cell dies). The −40 dB tail
gate is deliberately much hotter than the −70 dB silence gate so a released chord's
decay/reverb tail doesn't chain; a drone that never goes silent is what the toggle is
for (cancel a runaway chain by pressing the recording cell). Scenes use Ableton's default semantics (a
scene is a complete state of the band), and **arming a scene disarms every launch queued
outside its row** (an earlier scene, a single cell) — only the latest scene fires at the
boundary.

**Per-clip settings (`repeats`, `decay`, `after`) — how they are changed:**
- All three are **per slot** and live only in the **slot's right-click menu** (Rack's
  param context menu, extended): *Repeats* {∞, 1, 2, 4, 8, 16, 32, 64}, *Decay per
  repetition* (0 … −6 dB; gain = 10^(dB·repCount/20) per wrap), and *After* — the
  **follow action**: *Stop* (default) or *Play slot N* on the same track (the entry for
  the slot itself is marked "this: retrigger"). Editing in the menu never arms.
- **Selection = the last slot you pressed** (arm + select together), shown as a ring;
  it targets the continuous overdub, not the settings (the front-panel REPEATS/DECAY
  knobs are gone — settings are per-cell, not per-selection).
- **The slot shows its settings**: a corner tag (`∞`, `×4`; `↘` when decay < 0 dB;
  `→N` when a follow action is set); a playing finite slot counts down (`3 left`).
  Settings are editable on **empty cells too** — a follow action there makes the cell a
  **rest step** (the tag shows on an empty cell once a follow is set). Empty-cell
  settings persist as settings-only manifest rows (`"file": ""`), and *Clear slot* now
  resets a cell's settings along with its take. A UI-thread sweep (~2 Hz) reconciles
  every cell's settings into the manifest, so engine-side rewiring (auto-advance) and
  rest cells are never lost on save.
- **Defaults for new captures** are module-level (context menu: *New clips: repeats /
  decay*; default ∞ / 0 dB). A fresh capture always resets *After* to *Stop* — a stale
  follow from the cell's previous take never haunts a new one.

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
3. Playing slots wrap: `repCount++`; `gain = decay^repCount`; **done** if `repeats &&
   repCount == repeats` or `gain < 1e-3` (−60 dB). Repeat/decay edits apply at the wrap.
   A done clip runs its **follow action**: `after = 0` stops; `after = N` launches slot
   N on the same track at this very boundary (self = retrigger at full gain — repCount
   and gain reset; chains hop one clip per boundary, so cycles are safe). An **EMPTY
   target is a rest**: it "plays" silence for its own repeat count, then runs its own
   follow — chains may pass through gaps (a playing take-less cell demotes back to
   EMPTY, never to FILLED). A grid-mismatched target falls back to stop + a red flash
   on the target, and so does a RECORDING target (only possible when the auto-advance
   chain armed it this same boundary — the recording wins over the jump). An explicit launch/stop/capture committed at step 2 this same
   boundary already retargeted the playing slot, so user action always wins over the
   follow jump.

**Tempo conversion (BPM varispeed + BPI halving/doubling).** A **BPM change** within
0.5×–2× re-pitches takes, tape-style: the worker resamples each mismatched take to the
new interval length with a small windowed-sinc (16 taps/side, Hann, anti-alias cutoff
when slowing down) — pitch shifts with the tempo ratio, beats stay aligned. Bigger
jumps grey the takes (varispeed stops being musical), as does the context-menu toggle
*Tempo change: re-pitch takes* when off (re-enable + the next regrid re-derives).
Combined BPM+BPI changes decompose: resample for the tempo ratio, then the BPI
placement below. Everything shares the machinery described next.

**BPI conversion (halving/doubling at the same BPM).** A regrid greys mismatched
takes as before, but when the BPI exactly halved or doubled (same BPM, same sample
rate), the engine derives grid-fitting takes on the worker: **doubling tiles** the take
twice into the new interval (byte-identical to what the room heard); **halving splits**
it into two ×1-chained halves in the cell and the next EMPTY slot below (occupied → the
take stays grey) — an endless original becomes an A↔B cycle, so the pair replays the
phrase exactly. Derivations are **RAM-only**: nothing is saved, session.json keeps the
original take + settings (the widget sweep skips `derived` cells), so reload + regrid
re-derives from pristine sources. Results install through the **guarded** load path
(only if the source take / EMPTY target is still in place — a clear or re-record wins),
and derived takes are never derived again (they grey on further tempo changes; reload
restores originals). An overdub onto a derived take makes it real content: the derived
mark clears and the layered take saves to disk as usual. Limitations: finite repeat
counts can't span a split chain (the pair plays once, then the original's After) and
decay resets on every chain hop, as in any chain.

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

*(built, M4)* the take's OGG encode + `session.json`/`history/` writes are worker jobs:
the audio thread enqueues `SAVE{take buffer, meta}` on commit (capture / overdub) and
`CLEAR_FILE` on clear, over the same SPSC queue as the buffer commands. The worker calls
the engine's `LooperSink` (implemented by `src/looper/Session.{hpp,cpp}`, Rack-free) to
encode with the vendored OGG-Vorbis encoder and write the file atomically. Buffer-lifetime
safety: a take's `SAVE` is always ordered before its `RELEASE` on the one FIFO the single
worker drains, so the encode never reads a freed buffer (checked under ASan in
`looper_engine_test`). The encode (~100 ms/interval) can briefly delay buffer servicing,
but commits are user-paced and intervals are seconds long.

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
~/Music/jams/<YYYY-MM-DD_HHMM>_<room>/       (folder configurable in the Recorder's menu)
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
REC bezel button (arm; red light — same widget as the Looper's OVERDUB, aligned to it),
a "+TX" toggle, and a status panel: a state badge (RECORDING / ARMED / WAITING / IDLE),
the interval + size summary, and one row per source (green dot = a player, red = your
TX; name + interval count). 6 HP, with the "AK" mark. The jams folder defaults to
**~/Music/jams** and is changed from the right-click menu (Choose folder… / Reset /
Open jams folder), persisted in the Recorder's patch data. Status comes from
`RecorderLink` (dynamic_cast on the adjacent Ninjam); no Ninjam adjacent ⇒ the panel
says so.

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
 ├──────┼──────┼─ … ─┼──────┤        │
 │clip  │ clip │     │ clip │        │  8×8 clip grid            scene ▶ column
 │ grid │ grid │ …8× │ grid │        │  (right edge, not shown)
 │  …   │  …   │     │  …   │        │
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
  `session.json`. **Follow the mixer** (context-menu toggle, off by default): while on,
  the MindMeld MixMaster feeding our MULTI input names our tracks — the widget sweep
  walks the cable to the source module, reads its `trackLabels` JSON (4-char chunks,
  tracks first; the 16-track MixMaster's "1-8"/"9-16" direct-out port name gives the
  offset), and copies the names over ours (~2×/s; local renames are overwritten while
  following — that is the point). MixMaster's direct-out poly jack interleaves L/R
  exactly like MULTI, so labels map 1:1.
- **Scene** ×8 (▶ glyph, no number), **track STOP** ×8 (full-column-height raised
  buttons, visually distinct from the sunken clip cells) each with a **bi-color TX LED**
  beside it (`akDrawTxLedC` in Theme.hpp: green = on air → MIX, cyan = private → CUE),
  **STOP ALL**, **OVERDUB** latch: all params (MIDI-mappable). No per-track
  mute/level/pan — mute at the mixer, levels arrive set.
- Controls column right of the scenes, top → bottom: **OVERDUB** (component-library
  bezel button + red light, as Fundamental's PUSH), the poly **MULTI**
  input, then three compact stacked output **plates** (Theme `AK_PLATE_*` style): **CUE**
  over **POLY** over **MIX** (CUE/MIX are vertical stereo, L over R with the jacks squeezed
  close; POLY is a single poly jack between them). **POLY** is the per-track direct out:
  channels 2t / 2t+1 = track t L/R (mirroring the MULTI input's fixed pairs), each carrying
  that track's own output (its loop + gated live-thru, no limiter) — a clean per-instrument
  stem. The MIX plate bottom aligns with Ninjam's lower output-plate bottom, and the track
  **STOP** buttons grow down to the same line, so the two modules' output rows read
  together. The **AK** mark at the shared `AK_MARK_Y_MM`. No "SCENES" label; no RESET jack.
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

## 10. Session files  *(built, M4)*

```
<base>/<YYYY-MM-DD_HHMM>_<room>/                  one jam (base defaults to ~/Music/jams)
    looper/
        session.json
        t<track>_s<slot>.ogg                     ≤ 64 live files (raw takes, archive quality q≈0.8)
        history/<YYYYMMDD-HHMMSS>_<seq>_t<track>_s<slot>.ogg   overwritten/cleared takes (renamed, never deleted)
    players/ , tx/                               the Recorder's archive (§7.2)
```
Base folder defaults to **~/Music/jams** (same as the Recorder), configurable from the
Looper's right-click menu, persisted templated (`~/…`). The Looper and the Recorder
land in the **same jam folder** when a Recorder is armed: the Looper (UI thread)
`dynamic_cast`s its adjacent Ninjam to `RecorderLink` and borrows the exact
`<stamp>_<room>` Ninjam is archiving to, writing its `looper/` subfolder there. Without a
Recorder it forms its own `<stamp>_session/looper/`. The folder is **frozen once the
first take is written**, so arming a Recorder mid-jam never splits a session across two
folders. Directories are created lazily on the first real write — an empty session leaves
nothing on disk. **The whole grid persists as ≤64 raw OGG files and is restored on patch
reload — the clip loader (§11).**

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

## 11. Patch persistence + clip loader (`dataToJson`) *(built)*

Looper persists `simSecondsIdx`, `defRepeats`, `defDecayDb`, `trackNames`, the base
session folder (`sessionBase`), the **resolved session dir** (`sessionDir`), and the Rack
params (both folder paths templated `~/…` so a shared patch carries no user name). **No
audio in the patch** — but the session dir is, so the **clip loader** restores the grid on
reload:

- `dataFromJson` records the persisted `sessionDir` and flags a load; the widget's first
  `step()` runs `loadSession()` once (UI thread): parse `<dir>/session.json`, restore track
  names, seed the `Session` manifest model (`noteExistingTake`, so continued captures don't
  drop the restored takes), and `enqueueLoad` each take's OGG.
- The **worker** decodes each queued OGG (`stb_vorbis`, off the audio thread), allocates the
  buffer, computes the thumbnail, and hands it over via an SPSC `LoadInstall`.
- The **audio thread** (`drainLoads` in `tick`) installs each as a **FILLED** slot with its
  saved `repeats`/`decay` and thumbnail — playable once the live grid matches its length
  (the regrid rule; greyed until a matching clock arrives). Buffer lifetime is the usual
  rule — the worker frees it when the slot is later cleared/overwritten.
- A restored session **keeps its folder** (`sessionRestored`), so new captures land beside
  the loaded takes and share their manifest — until the user changes the base folder.

Recorder: "record own TX" toggle and armed state (re-arms on patch load only if Ninjam
auto-rejoins — it records nothing otherwise).

---

## 12. DAW export + deferred (v2+)

**DAW project generators.** The payoff of the shared timeline is a DAW project
(`.rpp` / `.als`) that reassembles the jam: the 8 Looper tracks with their takes, plus one
track per remote player and our TX with every interval at its `sessionFrame`, all
referencing the raw OGGs. The data on disk (`session.json` + `index.jsonl` + the OGGs) is
everything a generator needs.

The **Ableton `.als` exporter is built** (2026-08-25, `src/looper/AlsExport.{hpp,cpp}`,
offline test `test/als_export_test.cpp`) and — since the Recorder takeover the same
day — **owned by the Recorder module** (`src/JamExport.{hpp,cpp}` →
`akaudio::exportJamAls(jamRoot)`): a context-menu "Export Ableton Live set (.als)…"
(usable with no Ninjam adjacent — export only reads the jam folder) plus an automatic
export on recording stop (menu-toggleable; a ~1 s countdown lets the Looper worker
flush the disarm-boundary take — NjArchive::stop() already joins its writer, so
index.jsonl is complete). The Looper's own export menu item was removed: one owner.

**The as-played timeline.** The Looper engine emits a `LoopEvent` at every commit that
flips a cell's PLAYING state (launch, capture commit, follow jump, replaced, stop,
capture steal, mid-interval clear, regrid, exhaustion), stamped with
`ClockFrame::sessionFrame` — the same `JamClock::session` timeline `index.jsonl` rows
carry — plus the playing take's identity (`takeStartFrame`, its capture position).
Events ride the existing audio→worker SPSC queue (`Cmd::EVENT`, never blocks; a full
queue drops + counts) into `LooperSink::event` → `Session` appends JSONL rows to
`looper/events.jsonl` (`{"ev","t","s","sf","take","rest","bpm","bpi","sr","gen",
"reason"}`; pre-setDir events buffer bounded). The exporter (`buildLooperLanes`,
Rack-free) reconstructs per-track Arrangement lanes: tracks are monophonic (a START
closes any open span; an unpaired START closes at timeline end), a span becomes a clip
only when the manifest still holds a take with the same `(track, slot, startFrame)`
identity — re-recorded audio (retired to `history/` without metadata) is skipped, and
overdubbed spans deliberately reference the final overdubbed audio (Live's "clip as it
now is" semantics; overdubs keep `startFrame`). Rest spans (silence steps in follow
chains) are logged (`rest:true`) but produce no clip. Span clips loop their take
inside the span extent (`ClipSpec::spanBeats` vs `lenBeats`) — the exact shape Live's
own session→arrangement recording produces.

**Jam folder layout (2026-08-26):** date-grouped —
`<base>/<YYYY-MM-DD>/<HHMM>[_room]/` for Recorder jams and
`<base>/<YYYY-MM-DD>/<HHMM>_session/` for looper-only sessions.
`RecorderLink::recSessionName()` returns the jam folder RELATIVE to the base (it may
contain the date directory), and callers compose `base + "/" + name`. Adoption of a
same-run own `_session` folder uses **move semantics**: once the worker's byte-copies
all verify, the duplicate source's live files + manifest + events log are deleted and
its empty folders removed (`Session::migrateTo(dir, retireSource)`); restored sessions
and Recorder jam folders are never retired, a `history/` simply keeps the folder alive,
and any real copy failure cancels the retirement. The `.asd` files that appear beside
samples after opening a set are **Ableton's analysis cache**, not ours (disable via
Live Settings → File/Folder → "Create Analysis Files" if unwanted).

Arrangement positions are **rebased**:
the earliest archived frame (rounded down to a whole interval, so downbeats stay on
bars) becomes bar 1 — the session clock runs from the JOIN, and a jam armed minutes
into a room would otherwise start 100+ empty bars in (found 2026-08-25: first interval
at frame 17.5M ⇒ bar ~122, an "empty-looking" arrangement).
It is **attempt #2 — template surgery**, and the history matters: attempt #1 (2026-08-23,
commits `c74279c`/`a64f264`) synthesized the whole XML from scratch and was removed —
`.als` is gzipped XML with no official SDK, and a Live-openable file needs each track to
reproduce Live's full ~1000-line device serialization (`Mixer`, four routings, `Devices`,
`TakeLanes`, `FreezeSequencer`, `AudioSequencer`) with a consistent 700+-ID pointee graph;
short of that, Live's loader crashes building the tracks. Attempt #2 sidesteps all of
that: a real set saved by **Live 11.2 itself** — 8 audio tracks × 8 scenes + 2 returns,
exactly the Looper grid, authored for this purpose (`refs/Live11 Project/`) — ships
gunzipped as `res/als/Live11Template.xml`, and the exporter performs **targeted string
surgery** on it: rename the 8 tracks (session.json track names), clear the template's
demo clips (its Core-Library sample refs and the return-track preset paths — a home-dir
leak — are scrubbed; verified by test), splice our take clips into their `<ClipSlot>`s
(warped, `LoopOn`, two warp markers pinning `bpi` beats to the OGG's real duration —
Live imports Ogg Vorbis, so the raw takes are referenced with no re-encode; paths
project-relative with absolute backup), patch the master tempo, and — when the Recorder's
`index.jsonl` is present — append one Arrangement track per player + our TX by **cloning
the (cleared) donor track**, renumbering only its `AutomationTarget`/`ModulationTarget`
ids from `NextPointeeId`, every interval clip at its `sessionFrame`. Everything Live is
strict about stays Live's own bytes. Output: `<jamRoot>/<name>.als` beside `looper/`,
plus an empty **`Ableton Project Info/`** marker folder — without it Live doesn't treat
the jam root as a Project, won't resolve the project-relative sample paths, and reports
every OGG as missing media (it ignores the absolute `Path` backup too).

**Validated in real Live 11 Lite (2026-08-25)**: a 15-take session opens and plays —
grid, names, tempo, loops. Same day, the Arrangement side validated live-fire: an armed
Recorder's disarm auto-exported a set whose player + TX lanes appear in Live as
contiguous clips on the session timeline, audio decoded. Three iterations it took, each a Live error message:
(1) samples "missing" → FileRef `Type` must be `2` (audio file; `1` is wrong) and
`LastModDate` the file's real mtime; (2) "corrupt (Non-unique list ids)" → clip `Id`s
inside one Arrangement `<Events>` list must be sequential-unique, and a cloned track
must renumber its **whole pointee-space** — not just `AutomationTarget`/
`ModulationTarget` but `Pointee` and every `*ModulationTarget`
(Volume/Transposition/GrainSize/Flux/SampleOffset), all doc-globally unique;
(3) still "missing" → the `Ableton Project Info/` marker above; (4) "could not be
decoded using OggFLAC" on the first tx row → an archive armed **mid-interval** had
captured a headerless tail of the in-flight TX stream (the Ogg header pages went out
before the archive started). Fixed at the source — `NjClient` archives a TX interval
only if its BEGIN happened with the archive running (`txArchWhole`) — and the exporter
also skips any pre-fix on-disk row that doesn't start with a BOS + Vorbis ID header
(`standaloneOggFile`).
Schema notes (reverse-engineered, Live 11.2): unwarped clip Loop values are in seconds,
warped in beats; `CurrentStart/End` always beats; the first `<ClipSlotList>` per track is
the MainSequencer's (the FreezeSequencer has a second); the only `<Tempo><Manual>` lives
in the MasterTrack mixer. **REAPER `.rpp`** (plain text, ~100 lines) remains the sane
second target if another DAW is ever wanted; REAPER renders stems for anything.

**Deferred (v2+).** Loading clips from arbitrary files (the on-reload restore is built,
§11; loading a chosen OGG into a slot is not). Standalone clocking with per-track/per-take
interval lengths. Duplicate /
extend-with-silence (multi-interval loops). Per-slot "no stop"; tape-
style degradation. (Follow actions shipped: per-slot *After* = stop / play slot N.) History browse. FLAC slot files. NINJAM silence-interval TX.
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
|    | *Status 2026-08-23: implemented* — `src/looper/Session.{hpp,cpp}` (Rack-free `LooperSink`) + `test/session_test.cpp` (passes). Commits enqueue `SAVE`/`CLEAR_FILE` on the worker's SPSC queue; the worker encodes each take with the vendored OGG-Vorbis encoder and writes `t<t>_s<s>.ogg` atomically (tmp+rename), retiring an overwritten/cleared file into `history/`, and rewrites `session.json`. Buffer lifetime proven under ASan (`looper_engine_test` MockSink). The Looper borrows Ninjam's exact jam folder via `RecorderLink` when a Recorder is armed (else its own `<stamp>_session`), frozen after the first write. `sessionBase` (default `~/Music/jams`) is menu-configurable + persisted templated. The **clip loader** (§11) is also built: `Session::enqueueLoad`/`nextLoad` decode a saved OGG (stb_vorbis) on the worker, an SPSC `LoadInstall` hands it to the audio thread, and the persisted `sessionDir` triggers restore on patch load. `make unittest` builds + runs `session_test` (incl. the decode round-trip). | |
| M5 | Looper panel: thumbnail slot widget with live fill, header, context menu (quality, memory) | the UI |
| M6 | Docs (MANUAL.md, CHANGELOG), Library release | ship |

Tests (`test/`, no Rack link):
- `jamclock_test.cpp` *(built, passes)* — the integer `JamClock`: N vs NjAudio's
  formula, one downbeat + `bpi` beats per interval, timeline across a tempo change,
  fresh session on rejoin.
- `looper_engine_test.cpp` *(built, passes)* — drives `LooperEngine` with a synthetic
  clock + deterministic input: record-next-interval capture and launch/stop/overdub
  play back sample-exactly; a new recording stops the track's playing clip at its start
  boundary (takeover); a later scene press disarms an earlier one's queued launches; repeats;
  −6 dB decay; refused silent recording; scene stop; clear; regrid greys + refuses a
  mismatched take; CUE carries a private track's thru (not MIX); continuous overdub;
  loop declick; a submitted **`LoadInstall` installs as a FILLED slot and launches**;
  allocation count bounded (ASan-clean, incl. the load buffer).
- `session_test.cpp` *(built, passes)* — write a Looper session, check the OGG files
  (real `OggS` streams) + the `session.json` manifest (room, track name, per-slot repeats/
  bpm/`startFrame`), a late settings edit reflected, overwrite → the old take in `history/`,
  clear → the file retired + dropped from the manifest, no leftover `.tmp` (atomicity),
  an untouched session that writes nothing, and the **clip-loader decode round-trip**
  (a saved OGG decodes back to real audio at the right level, metadata preserved).
  Folded into `make unittest`.
- `archive_test.cpp` *(built, passes)* — feed `NjArchive` synthetic RX/TX intervals
  (incl. a silence interval that writes no file) with changing session frames; assert
  the per-player/tx files, verbatim bytes, JSONL index entries, and stats. `make unittest`
  builds + runs jamclock/looper_engine/archive together.
- Existing `enc_test.cpp`: add the silence-size assertion (≲ 8 KB per 20 s).
- Manual: Looper + Ninjam + Recorder on a real room; REAPER import of `clipsort.log`.
