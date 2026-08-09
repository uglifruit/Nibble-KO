# NIBBLE-KO

**A twelve-voice synth/sample percussion looper for the Music Thing Modular
Workshop System Computer, played by shift-and-tap on the Four Voltages
module — with a browser sample manager.**

*Four buttons. Ten voltages. Twelve drums, yours to fill.*

> **Status: in development, not yet playable.** This repository is currently
> a scaffold — ported and adapted code from two sibling cards, individually
> verified where a verifier exists, waiting on a design session before it
> becomes a working card. See [CLAUDE.md](CLAUDE.md) for exactly what's here
> and what isn't.

A program card for the [Music Thing Modular Workshop System
Computer](https://www.musicthing.co.uk/workshopsystem/) that expands the
Percussion half of [NIBBLE](https://github.com/uglifruit/WorkshopNibble)
into a dedicated drum machine: twelve voices, each independently synthesised
or sample-based, freely assigned and uploaded from a web interface — no
toolchain, no reflashing.

## Lineage

NIBBLE-KO combines two existing cards rather than starting from nothing:

- **The playing technique and looper** come from
  [NIBBLE](https://github.com/uglifruit/WorkshopNibble)'s Percussion half:
  Four Voltages' four non-latching buttons read as ten learned voltage
  levels, with a *shift* trick that recovers press-order from a signal that
  otherwise throws it away — hold one button as a bank select, tap another to
  play it, turning six two-button combinations into twelve distinguishable
  gestures. A four-bar event looper with lossless overdub sits behind it.
- **The sample management** comes from
  [BioMimicry](https://github.com/uglifruit/WorkshopBio)'s USB-MIDI SysEx
  upload pipeline: drop a WAV in the browser, it's converted, uploaded, and
  written into a reserved flash region that survives reflashing the firmware.

## Twelve voices, your choice of source

Every voice slot — kick, snare, hats, toms, cymbals, whatever you assign —
can be either a synthesised drum voice (phase-accumulator body, noise mix,
pitch sweep, the same DSP as NIBBLE's kit) or an uploaded sample, picked
independently per slot from the browser interface. Mix and match: real
kick and snare with synthesised hats, or a fully sampled kit, or the
reverse.

*(This per-voice source assignment is designed but not yet implemented —
see [CLAUDE.md](CLAUDE.md).)*

## Repo

`https://github.com/uglifruit/Nibble-KO` (public).

## License

CC BY 4.0 — see [LICENSE](LICENSE).
