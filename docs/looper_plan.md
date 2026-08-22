# Interval Looper — design handoff

> **Status:** decision log. The consolidated, canonical design is
> `docs/LOOPER_DESIGN.md`; read that first. Sections below record how each decision
> was reached (2026-08-22) and some are superseded by later ones.

Context document for a new module in the akaudio plugin. Written as a handoff
from a design conversation; treat decisions below as starting positions, not
final answers.

## Goal / use case

Live jamming over NINJAM (two players, e.g. with the akaudio Ninjam module).
Needs beyond what existing VCV loopers (Lilac Loop, Untitled Looper, Loopus)
provide:

1. **Queued recall:** return to any previous loop/take quickly, committed
   exactly at the *next interval boundary* (Ableton Session View semantics:
   press any time mid-interval, swap lands on the downbeat).
2. **Preservation:** every take automatically saved for later DAW editing —
   nothing is ever lost to overdub/erase. Ideally exports optimized for DAW
   import (per-take, per-track stems, timestamped).

## Decision: native module in akaudio, not a Lilac Loop fork

Rationale:
- akaudio is GPL-3.0-or-later — license-compatible with borrowing Lilac code
  (preserve copyright notices). `AudioFile.h` used by Lilac is Adam Stark's
  MIT library and can be used directly.
- The killer advantage: akaudio contains a NINJAM protocol client
  (`src/net/ninjam/NjClient` etc.). A looper in the same plugin can read
  BPM/BPI and interval position from protocol state — sample-accurate
  boundary commits with zero external CV plumbing, and correct behavior on
  mid-session BPM/BPI changes. No fork of a generic looper can do this.
- NINJAM's model is already interval-chunked audio (remote audio arrives as
  per-interval blocks; the official client saves sessions as per-interval
  files + index). "Slots of takes" is the native shape of the data.
- Interval-quantized recording simplifies the design vs. Lilac: loop length
  known a priori, buffers pre-sized, record start/stop snap to boundaries,
  smaller state machine (no free-length grow-as-you-record bookkeeping).

## Design sketch (starting position)

- **Takes as first-class slots.** Completed interval recordings push into a
  history (`std::vector` of buffer structs). Recording never destroys;
  erase = archive.
- **Arm/commit state machine.** Slot selection arms (`pendingSlot`); commit
  happens on interval boundary. UI shows live slot vs. armed slot.
- **Audio-thread safety for recall:** copy the recalled slot into a staging
  buffer on a worker thread at *arm* time; at the boundary do an O(1)
  `std::swap` of buffer internals in `process()`. Never allocate/memcpy
  multi-MB buffers in the audio thread. (Lilac's `MultiLoopWriter` shows the
  by-value-into-`std::async` snapshot pattern.)
- **Auto-export:** on take commit (and on erase/overwrite), async-write a
  timestamped WAV into a session directory. Multi-track for poly inputs.
  One writer handles one outstanding future — pool or queue for rapid takes.
- **Overdub semantics:** recalling a slot then overdubbing mutates a working
  copy, not the history entry (copy-on-arm gives this for free).

## Reference: Lilac Loop source (github.com/grough/lilac-loop-vcv, GPL-3)

Small, clean, worth reading before writing code (~2000 lines incl. tests):
- `src/modules/looper/engine.hpp` — Loop/MultiLoop buffer structs (171 lines)
- `src/modules/looper/module.hpp` — state machine, mode transitions in
  `toggle()`, autosave in `onSave()` (749 lines)
- `src/modules/looper/MultiLoopWriter.hpp` — async WAV export, by-value
  snapshot into std::async (181 lines)
- `src/modules/looper-feedback-expander/` — 73-line expander template; note
  Rack expanders are dumb jack panels, logic lives in the host module's
  `process()` polling `getRightExpander()`.
- Caution from its code: `write()` frees the path (strdup needed); output
  smoothing gates on mode changes to avoid clicks.

## Open questions for this session

1. **Coupling:** sibling module receiving interval clock via expander
   messaging from Ninjam, vs. looper as a direct expander flanking Ninjam,
   vs. reading shared client state another way. Start by reading how
   `src/Ninjam.cpp` structures/exposes NjClient state (BPM, BPI, interval
   sample position) and what its expander story currently is.
2. **Standalone fallback:** should the looper also work without the Ninjam
   module (external clock/phasor input defining the interval)? Probably yes,
   but don't let it complicate v1.
3. **Export format:** flat timestamped per-take WAVs vs. NINJAM-style
   session directory (per-interval files + index). What does the DAW-import
   workflow actually look like end to end?
4. **Slot UX:** how many slots visible, numeric display vs. lights, MIDI-map
   friendliness (jamming hands-free matters).
5. **Recording semantics:** always-record every interval (NINJAM-style,
   choose keepers later) vs. explicit record arm? Always-record matches the
   preservation goal and removes a performance-time decision.

## Suggested first steps

1. Read `src/Ninjam.cpp` and `src/net/ninjam/NjClient.hpp` — map out where
   interval timing lives and how a second module can see it.
2. Decide coupling (open question 1) and write it down here.
3. Scaffold the module (slug, panel placeholder, plugin.json entry) and get
   an interval-boundary trigger blinking a light from live protocol state —
   proves the sync path before any looper logic exists.
4. Then: record path → history → arm/commit swap → auto-export, in that
   order, with tests where the existing test/ setup allows.
   
## Decision: coupling (open question 1) — proposed 2026-08-22

### What Ninjam.cpp actually exposes today

Read of `src/Ninjam.cpp` + `src/net/ninjam/{NjClient,NjAudio}.hpp`:

- **No expander story exists.** Nothing in the plugin touches
  `leftExpander`/`rightExpander`/`onExpanderChange`.
- **There are three "interval clocks", and only one is the right one for us:**
  1. **The local beat clock in `Ninjam::process()`** (`beatPhase`, `beatIndex`,
     `spb = 60*sr/bpm`, `Ninjam.cpp` ~L771-845). Reset to beat 0 on `resyncBeat`
     (set by the net thread's `onConfig` on join and on every server tempo change —
     *immediately*, not at the next boundary). It drives the CLOCK/RESET/RUN/PHASE
     jacks, the metronome, and — crucially — **`armTransmit()`**, i.e. it is the grid
     our *uploads* are cut on. Remote clients hear our intervals on this grid.
  2. **`NjAudio::txLoop`'s cut** (`NjAudio.cpp` ~L640-660): after arming it counts
     `N = llround(bpi*60*sr/bpm)` integer frames per interval, continuously.
  3. **`NjAudio::mixLoop`'s playout** is *arrival-locked per remote channel* — there is
     no global receive boundary at all. Not a clock we can (or should) follow.
  So the looper must follow clock **1**, which is the thing the user hears as the
  downbeat (metronome) and the thing the room hears our audio cut on.
- `jamBpm`/`jamBpi` are atomics written by the net thread; `intervalSamples` lives
  inside `NjAudio` (private, int). Nothing is exported in a form another module can
  read lock-free *and* phase-accurately except the CV jacks.
- **Finding — clocks 1 and 2 disagree by a sub-sample fraction per interval.** The
  beat clock accumulates fractional `spb` (`beatPhase -= spb`), while the TX cut uses
  integer `N`. Whenever `bpi*60*sr/bpm` isn't an integer (e.g. 130 BPM·8 BPI @44.1k =
  162830.77) they drift ~0.2-0.5 samples/interval, i.e. tens of samples over a long
  jam. Inaudible for NINJAM, but a looper needs **one** integer interval length so
  every take is the same size and loops seamlessly. Recommended fix as part of this
  work: make the beat clock count integer frames (`frameInInterval++`, boundary at
  `N`, beat = `frameInInterval * bpi / N`) so RESET/PHASE/TX-arm/looper all agree
  exactly. Behaviour change for existing users is nil (sub-sample).
- Tempo is only defined in **JOIN** mode (`bpm > 0 && bpi > 0`); LISTEN mode has no
  grid → the looper has nothing to sync to there (see question 2).

### Options weighed

| | Pros | Cons |
|---|---|---|
| **A. Expander messaging** (Ninjam pushes a clock struct to an adjacent Looper each frame) | Rack-native, deterministic 1-sample latency, carries *structured* data (integer frame index, bpm/bpi, grid generation, joined flag, room label), no cables, no lifetime hazards (Rack nulls `expander.module` on removal) | Must be physically adjacent; one Looper per side (chaining is extra work) |
| **B. Looper as a "dumb" expander** (logic in Ninjam's `process()`, Lilac-style) | — | Backwards: the looper is the complex module; Ninjam.cpp is already 2300 lines. Rejected. |
| **C. Shared state another way** (pointer registry / module-id lookup / Ninjam globals) | No adjacency constraint | `APP->engine->getModule()` locks → unusable in `process()`; raw pointer sharing has no lifetime guarantee across patch load/delete; nonstandard. Rejected. |
| **D. Cables only** (existing RESET + PHASE jacks) | Works today; also *is* the standalone fallback for question 2 | RESET edge is sample-exact but PHASE is a float ramp (resolution ~1-2 samples over a long interval); can't carry bpm/bpi/generation/"joined"; user must patch it; a stray patch edit silently desyncs |

**Decision: A (expander messaging), with D reserved as the v2 standalone fallback behind the same internal interface.**

### Proposed design

`src/JamClock.hpp` — one header shared by both modules:

```cpp
// One frame of interval-grid state. Written by Ninjam::process() every frame into the
// adjacent Looper's message buffer; read by Looper::process() the next frame.
struct JamClockMessage {
    bool     running;          // joined to a room with a tempo (JOIN mode, bpm/bpi > 0)
    int      bpm, bpi;
    int      intervalFrames;   // integer N = llround(bpi*60*sr/bpm); 0 if !running
    int      frameInInterval;  // 0..N-1, integer (this is THE position)
    int      beatIndex;        // 0..bpi-1 (derived; for display / beat-quantized arming)
    bool     downbeat;         // frameInInterval == 0 this frame (interval boundary)
    bool     beat;             // a beat boundary this frame
    uint32_t gridGeneration;   // ++ on every resync (join, server tempo change, sr change)
    float    sampleRate;
    char     roomLabel[64];    // for export naming; NUL-terminated, copied every frame
};
```

Wiring (Rack's documented "push" convention):

- **Looper allocates** `leftExpander.producerMessage/consumerMessage = new JamClockMessage`
  *and* the same for `rightExpander`, so it works on either side of Ninjam. Frees them
  in its destructor.
- **Ninjam** refactors its beat clock into a `JamClock` struct (integer-counting, see
  finding above) whose `tick()` produces a `JamClockMessage`; Ninjam uses it for its own
  jacks/metronome/TX-arm, then for each side: `if (exp.module && exp.module->model ==
  modelLooper) { *(JamClockMessage*) exp.module->{left|right}Expander.producerMessage =
  msg; ...requestMessageFlip(); }`. A 90-byte struct copy per frame — negligible.
- **Looper** reads `consumerMessage` of whichever side has a `modelNinjam` neighbour
  (prefer left if both); if neither, it runs from its internal/fallback source.
- The looper's audio inputs are its own jacks (cable from the instrument chain, same
  signal the user patches into Ninjam IN, or the poly player outs). The expander
  carries *only* the clock. Passing audio through the message is deliberately out of
  scope.

What the looper does with it:

- **Boundary = `downbeat` flag.** Commit/recall/record-stop all happen on that frame.
  1-sample latency vs Ninjam's own cut is constant and deterministic (document it; no
  compensation needed — both modules see every frame, nothing is lost).
- **`gridGeneration` change ⇒ the grid moved.** Abort the in-flight recording (it's not
  interval-true anymore), cancel the armed recall, and request a buffer re-size on the
  worker thread for the new `intervalFrames`. Every history take is stamped with
  `(bpm, bpi, sampleRate, intervalFrames)`; recalling a take whose stamp doesn't match
  the live grid is refused (greyed) in v1 — no time-stretching.
- **`running` false ⇒ LISTEN mode / not joined:** looper idles (or uses fallback).

Internally the looper consumes an `IntervalSource` interface producing the same
`JamClockMessage`; the expander reader is the v1 implementation, and a cable-driven
one (RESET in + BPI/length param, the Lilac "pulse sets length" idea) is the v2
standalone answer to question 2 without touching the state machine.

### Consequences / follow-ups

- Ninjam.cpp changes are small and self-contained: extract the beat clock, switch it
  to integer counting, add a `gridGeneration` counter on the resync branch, add the
  expander write at the end of `process()`. No net/ layer changes.
- `onExpanderChange` isn't needed for correctness (we check `model` every frame), but
  the Looper can use it to flip a "synced to Ninjam" LED on the UI thread.
- Step 3 of "Suggested first steps" becomes: scaffold Looper with a single light driven
  by `msg.downbeat` — proves the whole path with ~30 lines in the Looper.

## Decision: tracks × slots, and multiple loopers — proposed 2026-08-22

**Requirement (from the jam setup):** 3-6 instruments, each with several loops/slots
that can be toggled independently.

**Shape: one *multitrack* looper module (tracks × slots grid), plus support for
several instances around Ninjam.**

- **Track = poly channel** (poly L/R `IN`, channel n = instrument n). Per track the
  output is live-thru (+ mute) mixed with the playing slot. The looper **submixes all
  tracks to one stereo MIX OUT** (per-track level + pan, soft limiter before the jack)
  and that single stereo pair goes to Ninjam IN — **one NINJAM channel on the wire,
  not one per instrument** (see "Submix, not stems" below). A poly OUT carries the
  raw per-track signals for anyone who wants them locally. v1: **8 tracks × 8 slots**
  (64 slot buttons; 16 tracks is the poly max but the panel doesn't want it).
- **Slot button = a Rack param** (momentary) + a light → MIDI-mappable via MIDI-Map/
  MIDI-CAT out of the box; a Launchpad-style 8×8 controller is the obvious hands-free
  surface. Plus per-track: record-arm, stop, live-thru/mute, **level + pan** (these feed the
  submix).
- **Why not per-instrument looper modules:** each needs HP and the clock, 6 of them
  must be contiguous, and "slots I toggle per instrument" is a grid anyway.

**Multiple instances: yes.** Two real uses: (1) >8 tracks or a second slot bank
(chain MIX OUT → next looper's IN to keep one wire channel), (2) the symmetric
setup — a Looper on Ninjam's **left** loops *your* instruments pre-TX; one on its
**right**, fed by Ninjam's poly **players** out, records what everyone else plays
(preservation applied to the whole room).

Mechanism (extends the coupling decision above): Ninjam **walks each side's chain**
every frame — `Module* m = rightExpander.module; while (m && m->model == modelLooper
&& hops++ < 8) { write JamClockMessage into m->leftExpander.producerMessage;
m->leftExpander.requestMessageFlip(); m = m->rightExpander.module; }` (mirror on the
left, using `m->rightExpander` as the Looper's receive buffer). Rack flips every
module's messages at the end of the same timestep, so **all Loopers in a chain see the
clock with a uniform 1-sample latency** — no relaying, no per-hop drift, Loopers stay
pure readers. Expander pointers are updated by the engine between blocks under its
lock, so chasing them from `process()` is safe. A non-Looper module breaks the chain
(stop there); a Looper that sees no Ninjam message falls back to its own source (Q2).

**Submix, not stems, on the wire (decided 2026-08-22).** The looper sends one stereo
submix to Ninjam rather than a channel per instrument. Why:
- *Bandwidth*, not server CPU: `ninjamsrv` never decodes or mixes — it relays OGG
  intervals byte-for-byte (plus optional session logging to disk) — so per-channel
  server CPU is ~nil even on a Celeron. What scales with channel count is your
  upstream (`channels × bitrate`, ~1.1 Mbps for 6 ch at the default quality), the
  server's fan-out (`channels × (clients-1) × bitrate`) + its log disk, the Vorbis
  *encode* CPU on our machine (6 encoders vs 1) and the *decode* CPU on every
  collaborator's machine.
- Stems exist anyway: the per-track auto-export (continuous per-track WAV = the stem
  set) can be shared after the jam. NINJAM isn't a stem-delivery channel.
- Owning the submix gives: what you hear = what they hear (monitor from MIX OUT), a
  limiter before the encoder so a loop stack can't clip on the wire, effects applied
  *pre*-looper so they're baked into the preserved takes.
- Known trade-off: collaborators lose per-instrument vol/pan of *your* channels on
  their end (canonical clients offer that per remote channel); they get one fader
  for you. Acceptable for a 2-player jam where you're the mixer.
- Consequence: `NjAudio::MAX_TX = 4` is **not** a constraint — no Ninjam change needed.
  (Non-goal: passing the submix through the expander message instead of a cable —
  same 1-sample latency as a cable, and it would have to merge with the IN jack.)

**Memory budget (drives the history design, Q3/Q5):** a stereo take = N × 8 bytes.
120 BPM·8 BPI @48k = 0.77 MB; 90 BPM·16 BPI = 4 MB; 60 BPM·32 BPI = 12 MB; the
existing sanity cap (1<<22 frames) = 33 MB. 8 tracks × 8 slots = **64 takes resident**
= ~50 MB typical, ~800 MB worst case — acceptable as the *slot* budget, but
"every take ever" cannot live in RAM: **the auto-export on disk is the history**, the
slot grid is the working set, and a slot can be reloaded from the session directory.

## Design document — outline + proposed answers (to confirm before implementing)

Target: `docs/LOOPER_DESIGN.md`, written from this plan once the open questions are
closed. Sections: goals & use cases · module topology (inline multitrack looper,
multi-instance, Ninjam changes) · clock coupling (`JamClockMessage`, chain walk) ·
track/slot model & state machine · threading (audio-thread rules, worker, staging
swap) · memory & history · export/session format & DAW workflow · persistence
(what goes in the patch, what on disk) · panel & MIDI · standalone fallback ·
milestones & tests.

Proposed defaults for the remaining open questions:

- **Q2 standalone fallback:** v1 = Ninjam-only; the looper internally consumes an
  `IntervalSource` so v2 adds a cable source (RESET in + beats/length param) without
  touching the state machine. Without any source the module idles with a "no clock"
  indicator rather than free-running.
- **Q3 export format:** session directory per jam
  (`<user>/akaudio-looper/<YYYY-MM-DD_HHMM>_<room>/`), one WAV **per take**:
  `t<track>_i<intervalIndex>_s<slot>.wav` + a `session.json` index (bpm/bpi/sr/
  interval length, take→slot map, timestamps). Flat per-take files import into any
  DAW as clips of exactly one interval; a NINJAM-style `.ninjam` session is *not*
  the target (no DAW reads it natively). Also write a per-track
  *continuous* WAV (the whole jam, gap-free): **yes** — that is the stem set shared
  with collaborators after the jam (see "Submix, not stems") and the "mix it later"
  source. Plus the MIX OUT as its own continuous WAV (= what went on the wire).
- **Q4 slot UX:** 8×8 grid of momentary buttons with tri-state lights (empty /
  holds take / playing; blinking = armed). Numeric interval counter + bpm/bpi
  readout in a header. No per-slot names on the panel (session.json carries
  metadata).
- **Q5 recording semantics:** **always-record** every interval into a rolling
  "last interval" buffer per track (cheap: one N-frame stereo buffer per track).
  A slot button on an *empty* slot = "keep the take that just ended" (post-hoc) or
  "record the next interval into this slot" (pre-arm) — both land on boundaries.
  Auto-export writes every interval that had signal, so nothing is ever lost and
  no record decision is required mid-performance. Overdub is v2.
- **Loop length:** v1 = exactly one interval. Multi-interval loops (×2, ×4) are a
  v2 flag on the take.

## Decision: two loopers, two roles — 2026-08-22

The symmetric setup is the v1 target: **left** of Ninjam an 8×8 grid looper with 8
stereo inputs for our instruments (submixed to Ninjam IN); **right** of Ninjam a
looper fed by the poly *players* out, recording the collaborators — one track per
player, **named from the roster**.

**Role is inferred from side.** The Looper sees which `Expander` the clock message
arrives on: message on `leftExpander` ⇒ Ninjam is to the left ⇒ **players role**;
on `rightExpander` ⇒ **instruments role**. Context-menu override exists; the default
needs no setup. The roles differ in three ways:

| | Instruments (left) | Players (right) |
|---|---|---|
| Boundary | local grid (`downbeat`) | **per-slot** (`playerBoundaryMask` bit s) |
| Track names | user-editable labels (default 1-8) | roster: `playerName(slot)` |
| MIX OUT | → Ninjam IN (the wire) | local **monitor only** — never back into Ninjam IN (room echo) |

**Why per-slot boundaries (critical, found in `NjAudio::mixLoop`):** received audio is
*arrival-locked per remote channel* — a collaborator's interval starts the moment it
lands (+ jitter hold) and chains from there, phase-true to *their* grid at an arbitrary
0-1 interval offset from *our* beat clock. Cutting on the local `downbeat` would slice
every collaborator's loop mid-phrase. The mix thread knows the exact frame each slot
starts a new interval (`curPos == 0` at pop), so it marks it; the players-role looper
cuts track s on that frame. Takes are then an *exact* re-slice of the collaborator's
uploaded intervals — uniform length (same room tempo), one per player per interval —
i.e. the server's session log, rebuilt locally as decoded WAV per player. A user with
several channels (e.g. guitar + voice) is one slot; mark on the slot's lowest-index
channel and ignore any further mark within N/2 frames (the looper debounces).

**Names stay out of the per-sample message.** They're needed by the panel and the
export worker, never the audio thread:
- Ninjam gains a mutex-guarded `std::string playerName(int slot) const` (NjAudio
  publishes slot→user under `mu`; today `userSlot` is private).
- The looper's widget `step()` (UI thread) reads it via the expander module pointer —
  safe because module removal also happens on the UI thread — into the looper's own
  guarded name table; the export worker reads that copy. Never from `process()`.
- `rosterGeneration` in the message tells the looper when to re-read.
- A NINJAM slot is **freed on leave and reused by the next arrival**, so track identity
  can change mid-jam: the take record stores the name at cut time and `session.json`
  logs slot→name changes.

**Message additions** (to `JamClockMessage`):
```cpp
uint16_t playerBoundaryMask; // bit s: remote slot s starts a new interval THIS frame
uint16_t playerActiveMask;   // bit s: slot s occupied (= Ninjam's poly channel count)
uint32_t rosterGeneration;   // ++ whenever slot<->name changes
uint8_t  side;               // which side of Ninjam this looper is on (role default)
```

**Ninjam changes (cumulative list for this project):**
1. Extract the beat clock into `JamClock` (integer frame counting) — see coupling.
2. Expander chain walk on both sides, writing `JamClockMessage`.
3. `NjAudio`: a lock-step **side channel on the wide ring** carrying the per-frame slot
   boundary mask (the mix thread sets bits when a slot's interval starts; `pullFrame`
   returns it alongside the frame). SPSC, same producer/consumer order → stays aligned.
4. `NjAudio`/`Ninjam`: `playerName(slot)` accessor + `rosterGeneration`.
No protocol, socket, or codec changes.

**Later (v2):** the players-role looper could also archive the collaborators' *raw
OGG intervals* (NjAudio already holds the bytes in `Transfer.bytes`) — lossless w.r.t.
what was sent and ~10× smaller than WAV — alongside the decoded takes. Not v1.

## Measured: Vorbis and silence (2026-08-22, vendored libvorbis, q=0.5, 48 kHz stereo)

Digital silence (exact zeros): **4.3 kbps** — almost all of it the ~4 KB per-stream
codebook header (NINJAM carries one per interval per channel); steady state ≈ 0.1 kbps.
But noise at **−90 dBFS costs 133 kbps** — the same order as full-scale noise (−60 dB:
162, −40 dB: 167) and *more* than tonal music (81). Vorbis's model is relative, so only
exact zeros are cheap; any analog noise floor or idling noise source is full price.
(WAV 32f stereo: 22.5 MB/min regardless.)

Implications:
- The TX submix **gates to exact zero** per interval (peak below ~−70 dBFS ⇒ emit
  zeros) so idle instruments cost ~nothing on the wire.
- Check whether `NjAudio::txLoop` can send a NINJAM **silence interval** (zero GUID,
  0 bytes — the receive side already handles them) for an all-zero interval instead of
  encoding it; today it encodes whatever it captured.
- Exports: WAV never compresses, so always-record **skips** intervals below the
  threshold; a v2 raw-OGG archive of collaborators is only small if *they* send true
  silence.

## Decision: slot play modes + recording semantics — 2026-08-22

**Play modes = two per-slot settings:** `repeats` ∈ {∞, 1…64} and `decay` ∈ (0, 1]
(gain multiplier per repetition). The three proposed modes are points in that space —
straight loop (∞, 1.0), gentle degradation (∞, <1), N-and-stop (N, 1.0) — and combine.
- Decay is a pure playback gain `decay^k` at repetition k; the take is **never
  modified** (history intact). A slot auto-stops once its gain is below −60 dB.
- Repeat/decay changes take effect at the loop wrap, so they're click-free.
- UI: two shared **REPEATS / DECAY** knobs (real params ⇒ MIDI-mappable) act on the
  *selected* slot (select = shift-click or long-press; selection shown by a ring).
- v2 on the same structure: follow actions (after N → next slot / random / stop),
  tape-style degradation (filter/wow) instead of plain gain.
- Loop-point click: takes are arbitrary cuts of continuous audio, so commit folds a
  ~5 ms equal-power crossfade of the head into the tail (worker thread, on the staging
  copy). Lilac-style output smoothing on start/stop as well.

**Recording: always, for every plugged-in track. No record arm.**
"Plugged in" has two levels, both used:
1. **Cable present** — `isConnected()` is `channels > 0` (Rack `Port.hpp`), a plain
   field read, safe in `process()`; Ninjam already sizes TX this way (`syncTransmit`).
   Panel has **8 stereo jack pairs**, one per track (not a single poly pair): plugged
   in = that track's L jack has a cable; R unconnected ⇒ mono (R = L). Track 1's pair
   also **fans a poly cable out** across tracks 2-8 (each track's own jack overrides
   its fan-out channel) so the players looper can take Ninjam's poly out with no
   Split. Caveat: a Merge reports channels up to its highest connected input, so gaps
   read "present" with 0 V — level 2 handles that.
2. **Signal present** — the per-interval peak gate (~−70 dBFS, see the Vorbis
   measurement). The only signal that the instrument is *playing*.
3. Players role: `playerActiveMask` (slot occupied). Rack clamps `setChannels(0)` to
   one channel of zeros, so Ninjam's poly out always looks connected — presence must
   come from the mask, not the channel count.

Rule: **record** every cable-present track (buffers are preallocated for all 8 tracks,
so this is free); **keep/export** a take only if its peak clears the gate; UI shows
present/playing per track.

**The capture gesture** (what always-record buys): press an *empty* slot while
interval k is playing → at the boundary, interval k — the material just played —
becomes the slot's take and starts looping immediately. No record press ahead of
time. Earlier intervals are reachable by a per-track history browse (knob/menu);
v1 if cheap, otherwise v2 (they're on disk regardless).

## Decision: loop length = one interval; sizing for long intervals — 2026-08-22

**v1: a loop is exactly one interval.** v2 loop-length ops on the same take model:
*duplicate* (×2/×4 the take) and *extend with silence*. No free-length recording.

**We jam at 32 BPI (4 bars for chord changes), possibly longer.** At 48 kHz that is
an interval of 16 s (120 BPM) – 21.3 s (90 BPM) – 32 s (60 BPM); a stereo 32f take is
**6–12 MB**, i.e. an order of magnitude above the earlier 8-BPI estimates. Consequences:
- **RAM:** 8 tracks × 8 slots fully populated = 0.4–0.8 GB (1.6 GB at 64 BPI). So
  slots are **allocated on demand** by the worker at capture time and freed on clear
  (never pre-sized as a 64-take array); the fixed cost is the always-record rolling
  buffer + last completed take per track = 16 intervals ≈ **100–200 MB** at 32 BPI.
  A memory readout in the context menu. Takes stay 32f (matches Rack + export; int16
  would halve it but costs precision on decaying loops — revisit only if needed).
- **Disk:** a full hour of 8 tracks always-recording at 32f stereo WAV would be ~11 GB
  if nothing is silent — superseded: the archive is **per-interval OGG Vorbis** (see
  "Decision: archive format" below), ~0.6-1 GB/hour for 8 busy tracks and ~nothing
  for silence.
- **Latency of the capture gesture:** "interval k becomes the loop at the next
  boundary" means up to a full 21 s wait if pressed early. That's the model; the
  boundary countdown on the panel (and Ninjam's progress bar) makes it predictable.
- **Ninjam cap:** `recomputeIntervalLocked` rejects intervals > 1<<22 frames (≈87 s at
  48 kHz, 95 s at 44.1 kHz) as a broken tempo. 64 BPI at 60 BPM (64 s) is fine;
  128 BPI at 60 BPM is not — raise the cap if the room ever goes there.

## Decision: archive format = per-interval OGG Vorbis, NINJAM session layout — 2026-08-22

**Measured** (vendored libvorbis as built by the plugin, Apple M4, 48 kHz stereo, 20 s
intervals): encode runs **220-600× realtime** per stream — **0.17 % of one core for
silence, ~0.4 % for an instrument, 0.46 % for white noise** at q=0.5 (q=0.3/0.8 are
within ±25 %). **Nine encoders (8 tracks + Ninjam's TX mixdown) ≈ 4-5 % of one core
worst case**; even a 10× slower CPU is ~40 % of one core, and the encodes run on the
looper's worker thread over whole-interval buffers (throughput, not latency). Not too
much. Sizes: ~1.2 MB/min per busy track at q=0.5, 32 KB/min silent.

**Format: the archive is a NINJAM-style session directory** — one `.ogg` per
(track, interval) + an index — not continuous WAVs:
- **No zeros are ever written.** A gated-silent interval = no file (the index records
  the gap); a partially silent interval costs ~0.1 kbps for its silent part.
- It is literally the format the NINJAM server logs (`clipsort.log` + per-interval
  OGG), so the **players-role** looper's archive is *the same thing* — and in v2 it can
  store the collaborators' raw received OGG bytes (`Transfer.bytes`) *as-is*: lossless
  w.r.t. the wire, zero re-encode. One format for both sides.
- **REAPER imports NINJAM session logs natively** (File → Open → `clipsort.log`, laying
  each interval on the timeline per user/channel) — verify on the first export. For
  other DAWs, a small "session → per-track stems" converter is a later tool.
- **Keepers = archive:** a slot references its interval file; nothing is written twice.
- Encoding uses the existing `encodeOggInterval()` (whole-interval function — exactly
  the unit we have). Ogg stream header overhead ≈ 4 KB/file: 170 intervals × 8 tracks
  ≈ 5 MB/hour, negligible.
- **Lossy, knowingly.** q=0.5 ≈ 160-190 kbps (the TX default); an **archive quality**
  setting defaults higher (q=0.8 ≈ 250 kbps, +60 % disk) because disk is cheap next to
  WAV. If lossless ever matters, FLAC is the alternative (silence also ≈ free, DAWs read
  it natively) at the cost of vendoring libFLAC or writing a fixed-predictor-only
  encoder — not v1.
- Our own instruments' tracks are encoded from the 32f capture (first generation); the
  TX mixdown that goes on the wire is still encoded by Ninjam as today.

## Decision: standalone clock (Q2) — 2026-08-22

- **Expander mode needs no clock jacks** — the grid comes from `JamClockMessage`.
- **No separate standalone module.** The same Looper runs standalone whenever no Ninjam
  neighbour is sending: `IntervalSource` = expander message if present, else the
  **RESET (downbeat) input jack**. Bonus: a looper that isn't adjacent to Ninjam can be
  clocked from Ninjam's existing RESET out by cable.
- **One RESET jack, not clock+reset or a phasor.** The looper acts only on interval
  boundaries — it has no beat-level behaviour (beat-quantized TX arming lives in
  Ninjam) — so a clock input would only serve display; a phasor (boundary = wrap) is a
  float ramp from whatever drives it, jittery/coarse next to an exact edge.
- **Interval length from a jack:** every slot's playhead **restarts on each edge**
  (phase-locks to the external clock, hides drift); takes are stored at the length
  actually recorded between edges and play up to the next edge. No learned-N, no
  free-length bookkeeping. Per-slot (player-role) boundaries don't exist in this
  mode — standalone = instruments role.
- **Panel: the RESET jack is on the v1 panel** (adding a jack later is patch-safe but a
  panel revision is churn). Wire it in v1 if it's the cheap edge detector it looks like;
  otherwise v2.

*Standalone addendum:* if/when a standalone mode is built, interval length becomes
configurable per instrument or per take — decided later; nothing in v1 should
preclude per-track interval lengths (i.e. keep N per track in the engine, not global).

## Decision: panel — Ableton Session layout, 8×8, scenes, thumbnails — 2026-08-22

**Layout (≈24-26 HP, final HP set by the SVG):** one column per track, a scene column
on the right, an I/O strip along the bottom.

```
 ┌─────────────────────────────────────────────────────────────┬───────┐
 │ LOOPER                       ● synced · 120 BPM · 32 BPI   12s │       │
 ├──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┬───────┤
 │ (L)  │ (L)  │ (L)  │ (L)  │ (L)  │ (L)  │ (L)  │ (L)  │       │   stereo jack pair
 │ (R)  │ (R)  │ (R)  │ (R)  │ (R)  │ (R)  │ (R)  │ (R)  │       │   per track (top)
 │ piano│ bass │ drums│ synth│  5   │  6   │  7   │  8   │       │   name strip
 │▂▅▇▅▂▁│▁▂▃▂▁ │      │      │      │      │      │      │       │   LIVE: interval-in-progress
 ├──────┼──────┼──────┼──────┼──────┼──────┼──────┼──────┼───────┤   filling left→right
 │[▁▃▇▅]│[▂▂▃▂]│[    ]│[▅▇▅▃]│[    ]│[    ]│[    ]│[    ]│ [ ▶ ] │ scene 1
 │[▂▅▂▁]│[    ]│[▇▇▇▇]│[    ]│[    ]│[    ]│[    ]│[    ]│ [ ▶ ] │ scene 2
 │  …   │  …   │  …   │  …   │  …   │  …   │  …   │  …   │  …    │ (8 rows)
 │[    ]│[    ]│[    ]│[    ]│[    ]│[    ]│[    ]│[    ]│ [ ▶ ] │ scene 8
 ├──────┼──────┼──────┼──────┼──────┼──────┼──────┼──────┼───────┤
 │ [■]  │ [■]  │ [■]  │ [■]  │ [■]  │ [■]  │ [■]  │ [■]  │ [■■]  │ track STOP / stop all
 │ ◦ ◦  │ ◦ ◦  │ ◦ ◦  │ ◦ ◦  │ ◦ ◦  │ ◦ ◦  │ ◦ ◦  │ ◦ ◦  │       │ level · pan (trimpots)
 │ [m]  │ [m]  │ [m]  │ [m]  │ [m]  │ [m]  │ [m]  │ [m]  │       │ mute (live-thru)
 ├──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┴───────┤
 │ MIX (L)(R)   POLY (L)(R)   RESET (in)   REPEATS ◯  DECAY ◯  ● archive │
 └─────────────────────────────────────────────────────────────┘
```

- **Slot buttons are rectangular** (~11 × 7 mm) and each draws a **waveform thumbnail**
  of its take. Fill encodes state: empty (flat), holds take (grey waveform), playing
  (accent waveform + moving playhead line), armed (blinking outline), selected for
  REPEATS/DECAY (white ring). Players role: the name strip shows roster names.
- **LIVE strip** under each name: the interval currently being recorded, filling left
  to right (Ableton's recording clip) — the visual answer to "what will I capture if I
  press now". Also shows the boundary countdown implicitly.
- **Scene buttons** (right column, one per row) launch the row, **Ableton default
  semantics**: every filled slot in the row is armed; a track whose slot in that row
  is *empty* is stopped (so a scene is a complete "state of the band"). A per-slot
  "no stop" flag is v2. Bottom-right: stop-all. All scene/stop/slot/mute buttons are
  params ⇒ MIDI-mappable (8×8 + 8 scenes + 9 stops + 8 mutes).
- Per track: level + pan trimpots (feed the submix) and mute (kills live-thru, loops
  still play). REPEATS / DECAY act on the selected slot.
- Bottom strip: MIX OUT L/R (→ Ninjam IN; "monitor" in players role), POLY OUT L/R,
  RESET in (standalone), archive LED (writing / error).

**Thumbnail data flow (keeps the audio thread clean):**
- A thumbnail = `THUMB_BINS` (64) × (min, max) of the mono-summed take, computed by
  the **worker** at commit time from the staging copy and stored with the take. The
  widget draws from a copy taken under the looper's UI mutex — never touches take
  audio.
- The **LIVE strip** is the one exception: `process()` accumulates the current
  interval's per-bin peak (one `fabs` + compare per frame into a fixed 64-float array
  indexed by `frameInInterval * 64 / N`); the widget reads it unsynchronized — a
  benign display race, reset at each boundary. No allocation, no lock.
- Playhead: per-track atomic `playPos` (0..1) written by `process()`, read by the
  widget.

## Decision: persistence = the grid, ≤64 OGG files per looper — 2026-08-22
*(supersedes "Decision: archive format" above: no per-interval archive, no index)*

**What is stored:** only the grid's recorded material — one `.ogg` per filled slot,
`t<track>_s<slot>.ogg`, plus `session.json` (bpm, bpi, sr, N, role, track names,
per-slot repeats/decay, timestamps). Takes are stored **raw**: level/pan/decay/mute are
non-destructive parameters in `session.json`, never baked in. A session directory
(`<user>/akaudio-looper/<YYYY-MM-DD_HHMM>_<room>/`, created at first capture) holds at
most 64 live files per looper (instruments and players loopers each get their own, or
`instruments/` + `players/` subdirs of one session).

**Writes:** on commit (capture or overdub) the worker encodes the slot from its **32f
RAM copy** (not from a decoded file — overdubs within a session stay first-generation)
and writes atomically (tmp + rename). **Clear/overwrite:** the old file is `rename()`d
into `history/<timestamp>_t_s.ogg` (default — zero cost, keeps "nothing is junk,
nothing is lost" without growing the live set) — or deleted, if we decide history is
unwanted. Overdub replaces the slot's content; the pre-overdub take survives only via
`history/`.

**Explicitly dropped from v1:** the always-record *archive* (the rolling buffer stays,
RAM-only, to feed the capture gesture), the NINJAM-style per-interval layout, REAPER
`clipsort.log` import, the players-role raw-OGG archive, history browse. Goal #2
("every take preserved") is now "every *kept* take preserved (+ history/ on
overwrite)" — a deliberate trade for a bounded, simple session.

**No clip loading in v1** (neither from files nor on patch reload). Consequence: a
patch reload or Rack autosave/crash recovery restores an **empty grid** while the
files remain on disk. v1 persists the session directory path in the patch so the
loader can restore it; **the loader is the first v2 item.** Overdub after a reload is
second-generation (decode → overdub → re-encode); the argument for FLAC slot files,
later.

**DAW import (v2):** the grid + `session.json` maps 1:1 onto a Session View. Ableton
Live imports Ogg Vorbis; `.als` is gzipped XML (undocumented but reverse-engineered:
8 audio tracks × 8 clip slots, clip length = N frames, tempo) — feasible. REAPER `.rpp`
is plain text and the easier first exporter.

**Buffer accounting (history/ costs no RAM — the old take's file already exists):**
- Per track, always: **2** rolling buffers (recording / last completed), pointer
  ping-pong at each boundary.
- Per filled slot: **1** take buffer (allocated on demand).
- Per *in-flight operation*: **1 transient**, allocated by the worker at arm time,
  pointer-swapped by `process()` at the boundary, released to the worker after:
  - *Capture:* the just-completed rolling buffer **becomes** the slot's buffer; the
    slot's old buffer (or a worker-pre-allocated fresh one for an empty slot) becomes
    the new rolling spare. Pure pointer rotation, no copy.
  - *Overdub:* worker copies the take to staging at arm; during the interval
    `process()` plays the take and sums input into staging; staging swaps in at the
    boundary. A second buffer exists only while that slot is overdubbing.
- Worker keeps a **free-list** of N-frame buffers (same size within a grid generation;
  flushed on `gridGeneration` change) so arming never waits on `malloc`.
- Worker **serializes jobs per slot** (encode old → rename → encode new), covering a
  re-capture that lands before the previous encode (~75 ms) finished.
- File ops on overwrite: `rename(t_s.ogg → history/<ts>_t_s.ogg)`, then encode the new
  take from its 32f buffer. No buffer is held for the old take.

## Correction: the right-side expander is a RECORDER, not a looper — 2026-08-22

The second module records, it doesn't loop. Its purpose: per-player `.ogg` files for
later inclusion in the DAW project (`.als`) generated from the Looper's session. This
simplifies everything: the Looper has **no players role** (no per-slot boundaries,
roster names, role inference — `playerBoundaryMask`/`playerActiveMask`/
`rosterGeneration`/`side` leave the message); the Recorder needs **no audio and no
decoding** — `NjAudio` already holds each received interval as the **raw OGG bytes**
(`Transfer.bytes`), and our TX intervals are already encoded, so the archive stores
*everything that crossed the wire, both directions*, lossless w.r.t. the wire at zero
encoder CPU. The archive object (`NjArchive`) lives in Ninjam (fed by `NjClient` on
its net/TX threads); the Recorder module is its panel via the expander pointer on the
UI thread. A monotonic `sessionFrame` in the clock message is the **shared timeline**:
Looper takes and received/sent intervals are all stamped on it (RX via
`mixFrameStart + pullOffset`, no wide-ring side channel needed), which is what lets
the `.als` place the whole jam as heard. See `docs/LOOPER_DESIGN.md` §7.
