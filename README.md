# NIBBLE-KO

**A twelve-voice synth/sample percussion looper for the Music Thing Modular
Workshop System Computer, played by shift-and-tap on the Four Voltages
module — with a browser sample manager.**

*Four buttons. Ten voltages. Twelve drums, yours to fill.*

> **Status: in development, playable on hardware.** Drums, the looper, mute
> groups, twelve performance effects and sample playback all work. Calibration
> is not yet saved to flash, so the card recalibrates on every power-up, and
> the browser sample manager is not written. See [CLAUDE.md](CLAUDE.md) for
> exactly what's here and what isn't.

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

## Calibration is the whole game

Patch **output 1** of Four Voltages into CV In 1, with its **knob at about
twelve o'clock**. Calibration runs at power-up: the LEDs show a combination,
you hold it and tap the switch to capture, ten times over.

> **Give it well-separated voltages, or nothing else will work properly.**

That knob moves all four outputs at once, and towards either extreme several
combinations collapse onto nearly the same voltage. When two learned levels
sit close together the symptom is not a clean failure but an intermittent
one: a pad that plays the wrong drum, fires twice, or does nothing — and does
it differently each time, because a few millivolts of drift moves the reading
across the boundary. It reads as a flaky card rather than a mis-set knob.

The card tells you when this has happened. **LEDs 4 and 5 blinking** during
calibration means that capture was too close to an earlier one. A run that
finishes with no warnings will behave predictably; one that warns will not —
so move the Four Voltages knob and calibrate again rather than trying to play
around it.

## Twelve voices, your choice of source

Every voice slot can be either a synthesised drum voice (phase-accumulator
body, noise mix, pitch sweep, the same DSP as NIBBLE's kit) or a sample.
Five are currently sampled from the Cheetah SpecDrum's original kit — kick,
snare, closed hat, open hat, clap — and seven stay synthesised. A mix, not a
wholesale swap: the synthesised voices are the ones whose Y-knob reshaping
earns its keep, and a bit-crushed synth tom next to a sampled clap is the
point of the card.

The Y knob is playback rate on a sampled voice and pitch-plus-decay on a
synthesised one — the same gesture either way, making the whole kit
higher-and-shorter or lower-and-longer.

Which sample a voice plays is a mapping of indices, not baked audio, so the
browser will be able to re-point it at runtime. See
[samples/README.md](samples/README.md).

## Repo

`https://github.com/uglifruit/Nibble-KO` (public).

## License

CC BY 4.0 — see [LICENSE](LICENSE).
