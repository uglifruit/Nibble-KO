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

## Current status: SCAFFOLD, NOT YET BUILDABLE

There is no `main.cpp` and the project does not compile to a UF2 yet.
What exists is ported/adapted logic, individually verified where a verifier
exists, waiting for a design session to turn it into a working card. See
"What's here" and "What's not here" below before doing anything else.

## Build

Toolchain comes from the Pico VS Code extension install at `~/.pico-sdk/`.
`CMakeLists.txt` includes `~/.pico-sdk/cmake/pico-vscode.cmake`, which pins
SDK 2.2.0 / GCC 14_2_Rel1 / picotool 2.2.0-a4. The `add_executable(...)` block
is commented out until `main.cpp` exists — see that file's TODO comment.

From PowerShell, once there is a `main.cpp` to build:

```powershell
$env:PICO_SDK_PATH = "$env:USERPROFILE\.pico-sdk\sdk\2.2.0"
$env:PATH = "$env:USERPROFILE\.pico-sdk\cmake\v3.31.5\bin;$env:USERPROFILE\.pico-sdk\ninja\v1.12.1;$env:USERPROFILE\.pico-sdk\toolchain\14_2_Rel1\bin;$env:PATH"
cmake -B build -G Ninja
cmake --build build
```

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
| `drums.h/.cpp` | Ported, synth backend only | NIBBLE's `drums.h/.cpp` — see its file header for what's TODO |
| `fastmath.h/.cpp` | Ported unchanged (namespace only) | NIBBLE's fixed-point helpers |
| `samples_default.h` | Written new | Adapted from WorkshopBio's mode×variant grid to 12 flat voice slots |
| `samplestore.h` | Written new, **not wired in** | Adapted from WorkshopBio; `ResolveSample()` has no caller yet |
| `webui.h` | Written new, interface only | Adapted from WorkshopBio's message set to 12 flat slots + new `MSG_SET_SOURCE` |
| `webui.cpp` | **Stub** — only `Encode7bit`/`Decode7bit` are real | Everything hardware-touching is a TODO |
| `tools/importwav.py`, `tools/mksamples.py` | Written new | WorkshopBio's WAV pipeline, adapted from mode×variant to 12 named voice slots |
| `tools/checksize.cmake`, `tools/bin2h.py` | Ported unchanged | WorkshopBio |
| `tools/ghostsim.py`, `loopsim.py`, `dspsim.py`, `syntax.sh`, `checkyaml.py`, `kittable.py`, `crosscheck.py` | Ported unchanged | NIBBLE — all pass against the ported `.cpp` files (see Verifying changes) |
| `ComputerCard.h`, `pico_sdk_import.cmake` | Vendored, byte-identical | NIBBLE — **do not edit** |
| `CMakeLists.txt` | Written new, mostly commented out | Sample-baking step wired; `add_executable` waits on `main.cpp` |
| `info.yaml` | Written new | `draft: true`, `Status: In development`, no UF2 |

## What's NOT here — the actual next work

1. **`main.cpp` does not exist.** No `ProcessSample()`, no boot sequence, no
   switch-gesture dispatch, no LED display. NIBBLE's DRUMS-mode code in
   `../WoskshopButtons/main.cpp` is the closest reference, but it shares the
   file with KEYS mode — NIBBLE-KO needs the equivalent written standalone.
2. **No sample playback.** `drums.h`'s `DrumVoice` has no PCM state. See that
   file's header comment for exactly what's undecided before writing it
   (position/fraction/increment fields, what the Y knob does to a sample
   voice, per-voice backend dispatch in `DrumKit::Step()`).
3. **No working WebUI protocol.** `webui.h`/`webui.cpp` are an interface
   shape and a stub. `MSG_SET_SOURCE`'s payload is undecided. No TinyUSB
   init, no SysEx dispatch, no flash writes.
4. **No `web/index.html`.** Deliberately not written — see `web/README.md`.
   There is no protocol yet for it to speak.
5. **No hardware testing.** Nothing here has touched a Workshop Computer.
   The ported logic passes its *software* verification models
   (`tools/*sim.py`), which is a much weaker claim than "works on the bench" —
   see `docs/LESSONS.md`'s own caveats about what those models did and didn't
   catch for NIBBLE.

**Recommended order for the next session:** design the `VoiceSource`
dual-backend (drums.h's TODO), then write `main.cpp` against the synth
backend only (so the card is playable and testable on hardware without
uploads), then design and wire the sample backend + WebUI protocol together,
since they're two ends of the same pipe.

## Why the namespace is `nko`, not `nib`

Every ported file's `namespace nib { ... }` was renamed to `namespace nko`
and `#include "nibble.h"` to `#include "nibbleko.h"`. Nothing else changed in
`levels.h/.cpp`, `looper.h/.cpp`, or `fastmath.h/.cpp` — they're logically
identical to NIBBLE's, just renamed to avoid colliding if both projects are
ever built against each other or diffed side by side.

## Layout

| Path | Purpose |
|------|---------|
| `main.cpp` | **Does not exist yet.** Card entry, `ProcessSample()`, boot latch, switch gestures, LEDs, output routing |
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

All pass as of this scaffold. `tools/syntax.sh` does not link, so `main.cpp`
not existing yet means nothing currently exercises the full include graph —
expect new syntax errors to surface once it's written.

## Repo

`https://github.com/uglifruit/Nibble-KO` (public). Commit as
Andy Jenkinson (uglifruit).
