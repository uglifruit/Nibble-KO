# NIBBLE-KO

**A twelve-voice synth/sample percussion looper for the Music Thing Modular
Workshop System Computer, played by shift-and-tap on the Four Voltages
module — with a browser sample manager.**

*Four buttons. Ten voltages. Twelve drums, yours to fill.*

> **Status: in development, playable on hardware.** Drums, the looper, mute
> groups, twelve performance effects, four pattern slots and sample playback
> all work. Calibration and patterns are not yet saved to flash, so both are
> lost at power-off, and the browser app is not written. See
> [CLAUDE.md](CLAUDE.md) for exactly what's here and what isn't.

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

## Playing it

The momentary switch is a **mode selector**, not a trigger. Its three
positions:

| Switch | |
|---|---|
| **Down** | choose a mode — hold it, press a button, and the choice lands when you **release** |
| **Middle** | play the chosen mode |
| **Up** | play it *and record* into the loop |

The mode **latches**, so Down and Up never need to be held at once — which is
what lets every mode be recorded through one mechanism.

| Hold switch, press | Mode |
|---|---|
| **A** | **Drums** — the kit (where you start) |
| **B** | **Mute** — three mute groups |
| **C** | **FX** — twelve performance effects |
| **D** | **Pattern** — four stored loops |

And with a **pair** held, one-shot actions:

| Hold switch, press | |
|---|---|
| **A + B** | **Undo** — revert the last recording pass |
| **A + C** | **WebUI** — hand the card over to USB *(not yet implemented)* |
| **A + D** | **Quantise** *(not yet implemented)* |
| **C + D** | **Play / Stop** |

> **Why singles commit on release but pairs fire instantly.** Four Voltages
> holds its last voltage, so the CV may already be sitting on the single you
> are trying to pick — pressing it produces no change at all, and a
> press-driven select would never fire. Reading the state on *release* is
> immune to that. A pair cannot get stuck the same way, because letting go of
> one falls back onto a single, so a pair press is always a real transition.

### Drums

Singles are **shifts**, not sounds. Hold one and tap another to play: twelve
ordered gestures from four buttons, because hold-C-tap-A and hold-A-tap-C are
different gestures on an identical voltage.

Main is a DJ filter (low-pass left, high-pass right, bypass at centre), X is
tempo, Y is kit character. All three are recorded when the switch is Up.

### Mute

Hold **B**, tap **A**, **C** or **D** to toggle one of three mute groups. The
pads show each group's hits going past — full brightness when it sounds, half
when it is muted, so you can see the pattern you have silenced and time
bringing it back.

### Pattern

Four slots. **Tap** a button to recall that pattern — instantly, keeping the
playhead where it is, so switching mid-bar reads as the band changing part
rather than a stop and start. **Hold** a button for a second to store the
live loop into it.

The pads show three states: **full** is the pattern playing, **half** is a
slot with something in it, **dark** is empty — so what you can jump to is
readable at a glance.

Recalling **discards** whatever you were playing. That is deliberate, and it
is why storing is a separate hold: it means you can try something over a
pattern and abandon it. (Undo still gets you back one step.)

### FX — twelve effects, four banks

Hold any single as a shift, press another for its effect. The shift picks the
**family**, which is what makes twelve gestures learnable as four groups:

| Hold | Tap | Effect | Main knob |
|---|---|---|---|
| **A** *filters* | B | Low-pass | cutoff |
| | C | High-pass | cutoff |
| | D | Band sweep | centre |
| **B** *destruction* | A | Bit-crush | bits discarded |
| | C | Decimate | sample-rate reduction |
| | D | Wavefold | drive |
| **C** *rhythmic* | A | Stutter | repeat rate |
| | B | Flam | gap, 5–35 ms |
| | D | Gate | chop rate |
| **D** *transport* | A | **Reverse** | — |
| | B | **Tape-stop** | fall time, 50 ms–2.4 s |
| | C | Silence | — |

Effects are **momentary** — held, not toggled — and one per bank, so up to
four layer at once, processed in series.

**The transport bank means different things to different voices**, which is
the point of grouping it: the gesture is "play it wrong on purpose", and what
that means depends on what the voice is made of.

| | Sampled voice | Synthesised voice |
|---|---|---|
| **Reverse** | plays backwards | envelope backwards — a *swell* |
| **Tape-stop** | rate winds to nothing | *pitch* winds to nothing |

Since five voices are sampled and seven synthesised, one reverse press gives
you backwards kick and hats over swelling toms and cymbals at the same time.

Both are armed at **trigger time**, so they catch hits that start while the
effect is held and leave anything already sounding alone.

### Recording effects

Each shift owns **two** recorded lanes — which effect is running, and that
lane's parameter curve — performed separately:

- **shift + tap**, switch Up: records the effect popping in and out
- **shift alone** + Main, switch Up: draws that lane's parameter curve

So you can sweep a filter curve once, then punch the effect in and out over
the top of it on later passes without re-recording the sweep. Turning Main
with *no* shift held does nothing, deliberately: with four curves and no
button down, which one you meant is ambiguous.

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

## Panel

| | |
|---|---|
| **CV In 1** | one output of Four Voltages — the instrument |
| **Pulse In 1** | external clock, one edge per beat |
| **Main** | DJ filter, or FX depth while in FX mode |
| **X** | tempo, 40–240 BPM (ignored while clocked) |
| **Y** | kit character — pitch and decay on synth voices, playback rate on samples |
| **Audio Out 1 / 2** | the drum bus, mono, both the same |
| **Pulse Out 1** | gate on every hit |
| **Pulse Out 2** | click, one per beat |
| **LED 4 / 5** | recording / beat |
| **CV Out 1 / 2** | unassigned |

## What isn't done

- **Calibration is not saved.** The card recalibrates at every power-up. The
  flash write is the next substantial piece of work.
- **No browser sample manager.** The USB/SysEx layer is a stub, so samples are
  baked at build time rather than uploaded.
- **Quantise does nothing** yet.
- **Patterns are RAM only** — they do not survive a power cycle. Getting
  them in and out over the web app is the next natural step.
- **Mute groups are hardcoded** three ways by voice index, pending the WebUI
  that would assign them.

## Repo

`https://github.com/uglifruit/Nibble-KO` (public).

## License

CC BY 4.0 — see [LICENSE](LICENSE).
