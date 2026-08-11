# NIBBLE-KO — working notes for Claude Code

A program card for the **Music Thing Modular Workshop System Computer**
(RP2040), built on the header-only **ComputerCard** library. Sibling project
to `../WoskshopButtons` (NIBBLE, repo `WorkshopNibble`), `../WorkshopBio` and
`../WorkshopZX` — reuse their conventions and structure where they fit.

**NIBBLE-KO is the expanded percussion half of NIBBLE**: a twelve-voice drum
machine reading one output of the Workshop System's Four Voltages module,
where every voice is independently synthesised or sample-based, chosen and
uploaded from a browser WebUI — the sample-management pattern from
`../WorkshopBio`.

## Current status: PLAYABLE, TESTED ON HARDWARE

Builds to `build/nibbleko.uf2` — **6.6% flash, 14.2% RAM**. Confirmed working
on a Workshop Computer: drums, the four-bar looper with lossless overdub,
three mute groups, twelve performance effects with two-lane recording, three
pattern slots, and sample playback from a baked bank.

**Nothing is saved.** Calibration and patterns are RAM only, so both die at
power-off, and the card recalibrates at every boot. That plus the browser app
is the bulk of what is left — see "What's NOT here".

## Build

Toolchain comes from the Pico VS Code extension install at `~/.pico-sdk/`.
`CMakeLists.txt` includes `~/.pico-sdk/cmake/pico-vscode.cmake`, which pins
SDK 2.2.0 / GCC 14_2_Rel1 / picotool 2.2.0-a4.

From PowerShell:

```powershell
$env:PICO_SDK_PATH = "$env:USERPROFILE\.pico-sdk\sdk\2.2.0"
$env:PATH = "$env:USERPROFILE\.pico-sdk\cmake\v3.31.5\bin;$env:USERPROFILE\.pico-sdk\ninja\v1.12.1;$env:USERPROFILE\.pico-sdk\toolchain\14_2_Rel1\bin;$env:PATH"
cmake -B build -G Ninja
cmake --build build
```

Output: `build/nibbleko.uf2`. Copy to `FLASHME/` for flashing (git-ignored).
`cmake`/`ninja` are **not** on the default PATH — always set it as above.

## Hard rules

Identical platform constraints to NIBBLE and WorkshopBio — see
`docs/LESSONS.md` §3 for the full list with the reasoning. The short version:

- `ProcessSample()` runs at **48 kHz** on core 0, inside a DMA interrupt.
  Allocation-free, no `malloc`, no blocking, no `float` in the hot path —
  fixed-point only.
- Audio/CV I/O is signed 12-bit (`-2048..2047`). `KnobVal()` is unsigned 12-bit
  (`0..4095`).
- **Never** do hardware setup in the `ComputerCard` constructor — it wedges the
  chip. Setup goes in `main()`.
- `PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64` is required for the Workshop
  Computer's crystal. Don't remove it.
- The switch reads **Down for the first few milliseconds of every boot**
  (ComputerCard derives it from `knobs[3]`, off a ~60 Hz filter starting at
  zero, and zero decodes as Down). Latch any alt-boot from **one** reading
  after a settle window — never "Down seen at any point". Three sibling cards
  have shipped that bug between them.
- `CVOutMillivolts()` / `CVOutMIDINote()` are **flash-resident**. Cache the
  last value and only call them on a change, or they put XIP reads in the hot
  loop.
- If this card ends up carrying USB (it will, for the WebUI), writing flash
  while ComputerCard runs **will hang the card** unless the five-step
  protocol in `docs/LESSONS.md` is followed exactly. Read it before touching
  `webui.cpp`'s stubbed flash-write methods.

## What's here

Ported and adapted from `../WoskshopButtons` (NIBBLE) and `../WorkshopBio`
(BioMimicry), namespace `nko` throughout (NIBBLE's was `nib`):

| Path | Status | Source |
|------|--------|--------|
| `nibbleko.h` | Ported, trimmed | NIBBLE's `nibble.h`, minus `BootMode`/scale vocabulary (KEYS-only) |
| `levels.h/.cpp` | Ported unchanged | NIBBLE's level detector + **the ghost rule** + `Shift()` |
| `looper.h/.cpp` | Ported unchanged | NIBBLE's event looper |
| `drums.h/.cpp` | Ported + PCM backend | NIBBLE's synth engine, plus sample playback and the D-bank transport effects |
| `fastmath.h/.cpp` | Ported unchanged (namespace only) | NIBBLE's fixed-point helpers |
| `samples_default.h` | Written new | The baked sample BANK plus `kVoiceSample`, the voice→bank mapping |
| `samplestore.h` | Written new, wired in | `ResolveSample()` is called per hit from `DrumKit::TriggerVoice` |
| `webui.h` | Written new, interface only | Adapted from WorkshopBio's message set to 12 flat slots + new `MSG_SET_SOURCE` |
| `webui.cpp` | **Stub** — only `Encode7bit`/`Decode7bit` are real | Everything hardware-touching is a TODO |
| `tools/importbank.py`, `mksamples.py` | Written new | WAV → numbered bank entries; `importwav.py` supplies the DSP |
| `fx.h/.cpp` | Written new | Twelve performance effects, four slots chained in series |
| `tools/checksize.cmake`, `tools/bin2h.py` | Ported unchanged | WorkshopBio |
| `tools/ghostsim.py`, `loopsim.py`, `dspsim.py`, `syntax.sh`, `checkyaml.py`, `kittable.py`, `crosscheck.py` | Ported unchanged | NIBBLE — all pass against the ported `.cpp` files (see Verifying changes) |
| `ComputerCard.h`, `pico_sdk_import.cmake` | Vendored, byte-identical | NIBBLE — **do not edit** |
| `main.cpp` | Written new | The mode machine, Drum Performance, LEDs, calibration. Structure follows NIBBLE's `main.cpp` closely |
| `CMakeLists.txt` | Written new | Builds; `webui.cpp` deliberately excluded until it is real |
| `info.yaml` | Written new | `draft: true`, `Status: In development` |

## The control surface

The switch is a **mode selector**, not a trigger — this is the single biggest
departure from NIBBLE, and `main.cpp`'s file header explains it in full.
Briefly: hold the switch **Down** and press a button to choose a mode, which
**latches** on release; then **Middle** plays that mode and **Up** plays and
records it. Latching is what lets every mode be recorded through one
mechanism, since Down and Up never need to be held at once.

    switch+A  DRUMS (default)    switch+AB  UNDO
    switch+B  MUTE               switch+AC  WebUI setup
    switch+C  FX  (twelve)       switch+AD  QUANTISE
    switch+D  PATTERN (three)    switch+CD  PLAY/STOP

**Singles commit on RELEASE; pairs fire IMMEDIATELY.** That asymmetry is not
an oversight and should not be "fixed" — both halves fall out of the ghost
rule. Four Voltages holds its last voltage, so the CV may already be sitting
on the single you are trying to select, in which case pressing it produces no
transition and a press-driven select would never fire. Reading the *state* on
release is immune to that. A pair cannot get stuck the same way, because
releasing a pair falls back onto one of its members and `levels.cpp` sets
`current_` to that **single** — so the resting state is never a pair, and a
pair press is always a genuine transition. See `main.cpp`'s header.

## Gotchas — the ones that have already bitten

Every one of these was found on hardware, and most were invisible in the
code. They are here because the same shapes will recur.

### FOUR VOLTAGES LATCHES. Almost everything traces back to this.

The output holds the last-pressed level indefinitely. There is no rest
voltage, and a "held" button is indistinguishable from one pressed and
released. Three separate bugs came from forgetting it:

- **A hold-to-store gesture cannot work.** Any "held for N ticks" test passes
  eventually, because the level just sits there. The pattern store was
  written this way and every recall became a store. The switch decides the
  verb instead.
- **A press-driven mode select never fires** when the CV is already on the
  button you want. Hence commit-on-release for singles.
- **A bare-press toggle can turn something on and never off**, because
  pressing the same button twice needs a transition that never happens.
  Hence shift-and-tap for mutes.

### Ordering inside ControlTick is load-bearing

`FireCombo()` runs BEFORE `PlayControl()`, so anything `PlayControl()`
assigns is one tick stale when a button handler reads it. That is harmless
for values that only play, and destructive for values that decide whether to
*write*: `PatternPress` read `recording_` this way and a stale-true reading
silently overwrote a pattern slot. **Read `SwitchVal()` directly** in a
button handler rather than the cached flag.

### `Current()` versus `Sounding()` is not a style choice

`Sounding()` reports the pair for as long as the ghost is armed — i.e. after
the tapping finger has lifted. Right for a DRUM, whose hit is still ringing.
Wrong for anything momentary: FX read `Sounding()` at first and effects
stayed latched after release, and re-tapping the same button produced no
change at all, so one whole bank appeared not to work.

### Replacing the event array invalidates the cursor

`cursor_` is an index into `events_`. Undo and pattern recall both replace
that array wholesale, so it must be **rebuilt from the playhead**. Resetting
it to zero looks safe and makes `Fire()` re-fire everything between the loop
start and the current position, all on one tick.

### The Python models are not decoration

They have caught, before hardware: a stalling slew, a 0.33x soft clip, a
looper sorting by the wrong key, a double-fire, and a lane-encoding drift
where `loopsim` stored raw knob values while `looper.cpp` stored `knob >> 4`
— invisible until the FX lane packed data into all eight bits. **If you
change `levels.cpp`, `looper.cpp` or `drums.cpp`, change the model too**, or
delete it rather than let it lie.

### Units: loop ticks are not control ticks

Eight loop ticks is 42ms at 240bpm and 250ms at 40bpm — 125 versus 750
control ticks. Anything measured in one and counted in the other is wrong
across most of the tempo range. The FX playback hold was written in control
ticks first and expired instantly at slow tempos.

## What's NOT here — the actual next work

Roughly in dependency order:

1. **Nothing is saved to flash.** Calibration and the three pattern slots are
   both RAM only, so both die at power-off. Two pieces of temporary
   scaffolding exist around the calibration half, marked `TODO(calibstore)`:
   - **Every boot calibrates**, not just the alt-boot. Without saved levels a
     normal boot would come up on the evenly-spaced *default* spread, which
     is not a real calibration and cannot be played.
   - **Abort and timeout RESTART the learn** rather than exiting, because
     exiting would land on that same unplayable default. Once levels can be
     restored, aborting should go back to meaning "keep what I had".

   Needs a small `calibstore.h` (sibling to `samplestore.h`, own 4KB sector)
   and the five-step flash protocol from `docs/LESSONS.md`. **Highest-value
   next task** — it is what turns the card from "recalibrate every time you
   power it on" into the design that was agreed.

2. **No WebUI.** `webui.h` is an interface shape, `webui.cpp` a stub (only
   the 7-bit codec is real), and it is **not in the build**. No `web/`
   client. Three things want it, in rough order of value:
   - **pattern transfer** — a pattern is a bare event list under 2KB and
     carries no audio, so this is close to a straight dump over SysEx
   - **user samples into flash**, addressed by bank index like the baked ones
   - **voice/sample assignment, mute-group assignment, loop setup** (bars,
     swing, quantise grid) — all currently compile-time tables

3. **Quantise does nothing.** The gesture (switch+AD) is wired and dispatches
   to `FireAction()`; `Looper` needs a `QuantiseNow()` and a runtime-settable
   grid. Undo, by contrast, is done.

4. **Mute groups are hardcoded.** `MuteGroupOf()` splits the kit three ways
   by voice index so the mode is testable; the real mapping belongs in the
   WebUI alongside sample assignment.

   Note that mutes NOT being recorded is a decision, not a gap — see the
   note in `MutePress`. A mute is a mixer move rather than part of the
   music, so it stays card state, persists across pattern recalls, and is
   outside Undo. Do not "finish" it.

5. **`kVoiceSample` is compile-time.** Five voices are sampled and seven
   synthesised, fixed at build. The indirection is already right for a
   runtime table — it is indices, not audio — so this is a load rather than
   a redesign.

## Known gaps in what IS written

Things that compile and look finished but are not, worth knowing before
trusting them:

- **Reverse on a synth voice is hard to judge.** The swell runs over roughly
  the length the decay would have taken, derived from each voice's decay
  shift — one expression in `SetReverse()`. Whether that ratio is musical
  across the kit is unsettled.
- **`kSelectMinTicks` (50ms) is a guess.** It is the debounce that stops a
  knock against the switch re-latching the mode.
- **Gate is not tempo-synced.** Its rate divides the sample clock, so it does
  not lock to the loop. Deliberate — a gate that slowed with the pattern is
  just tremolo — but worth revisiting.
- **CV Out 1 and 2 output nothing.** Deliberate — NIBBLE's bassline is gone
  and no replacement has been chosen.
- **An intermittent fault is recorded in `docs/OPEN-BUGS.md`**, seen once and
  never reproduced.

## Why the namespace is `nko`, not `nib`

Every ported file's `namespace nib { ... }` was renamed to `namespace nko`
and `#include "nibble.h"` to `#include "nibbleko.h"`. Nothing else changed in
`levels.h/.cpp`, `looper.h/.cpp`, or `fastmath.h/.cpp` — they're logically
identical to NIBBLE's, just renamed to avoid colliding if both projects are
ever built against each other or diffed side by side.

## Layout

| Path | Purpose |
|------|---------|
| `main.cpp` | Card entry, `ProcessSample()`, boot latch, the mode machine, switch gestures, LEDs, output routing |
| `nibbleko.h` | Shared constants, combo indices, rates, LED helpers |
| `levels.h/.cpp` | Level detection, settle/match, **the ghost rule**, `Shift()` |
| `drums.h/.cpp` | Twelve voices: synth engine (working), sample backend (TODO), DJ filter |
| `looper.h/.cpp` | Event loop: record, overdub, tempo, external clock |
| `samplestore.h` | Flash layout for user-uploaded samples, per voice slot (not wired in) |
| `samples_default.h` | `__has_include` shim so the build works with or without baked samples |
| `webui.h/.cpp` | USB-MIDI SysEx transport + upload state machine (interface + stub) |
| `web/` | Browser sample manager — not written yet, see `web/README.md` |
| `fastmath.h/.cpp` | Fixed-point helpers, sine LUT, PRNG |
| `ComputerCard.h` | Vendored MTM library — **do not edit** |
| `tools/` | Python verification models, `syntax.sh`, `checkyaml.py`, sample pipeline |
| `info.yaml` | Workshop System card registry metadata (`draft: true`) |
| `docs/LESSONS.md` | NIBBLE's handover doc — **read this first**, most of the reasoning behind what's ported here lives there |

## Starting work on this card

Read **`docs/LESSONS.md`** first — it's NIBBLE's handover, written explicitly
anticipating this card ("NibbleDrumMachine": percussion only, with real
samples). §4 in particular ("For NibbleDrumMachine specifically") is the
closest thing to a design brief that exists: what to take wholesale, what
NIBBLE deliberately chose that should be *reconsidered* for a sample-based
card (not inherited blindly), and the exact five-step flash-write protocol
for uploads.

## Verifying changes

**There is no host C++ compiler on this machine.** Same two things fill the
gap as on NIBBLE:

```sh
sh tools/syntax.sh          # type-check every .cpp with the ARM compiler, ~1s
python tools/ghostsim.py    # the ghost rule + learn round-trip
python tools/dspsim.py      # DJ filter stability, soft clip
python tools/loopsim.py     # event ordering, overdub, tempo
python tools/checkyaml.py   # info.yaml parses AND is structurally complete
```

All pass, and the card builds clean with `-Wall -Wextra -Wdouble-promotion
-Wfloat-conversion`. `tools/syntax.sh` does **not** link, so it cannot catch a
missing symbol — run a real `cmake --build` before believing anything.

The Python models are **line-by-line ports** of the C++ they mirror. If you
change `levels.cpp`, `drums.cpp` or `looper.cpp`, change them too — or delete
them rather than let them drift into telling you a comfortable lie. Between
them they caught four real bugs on NIBBLE that would each have been hard to
diagnose by ear.

Nothing yet models `main.cpp`'s **mode machine** — the commit-on-release
select, the pair-fires-immediately path, the record-arm transition. That is
the obvious next thing to model, and the reasoning is the same one that
motivated `ghostsim.py`: it is ordering-sensitive logic where a wrong answer
is silent.

## Repo

`https://github.com/uglifruit/Nibble-KO` (public). Commit as
Andy Jenkinson (uglifruit).
