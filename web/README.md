# web/

`index.html` is the browser setup tool for NIBBLE-KO. It is a single
self-contained file — no build step, no dependencies, no server. Open it
locally, or serve the directory over `http://localhost`, and it talks to the
card over WebMIDI SysEx.

## Using it

1. On the card, hold the switch **Down** and press **B + D**. All four pads
   glow slowly: the card is on USB and silent.
2. Open `index.html` in **Chrome or Edge** (Safari and Firefox have no
   WebMIDI) and click **Connect USB**.
3. The card appears as a MIDI port named `NIBBLE-KO (Workshop)` — the page
   finds it by that name, so if `usb_descriptors.c`'s product string changes,
   change `DEVICE_RE` in `index.html` to match.

The card only enumerates once it is in WebUI mode, so "card not found"
usually means step 1 has not happened yet. The page watches for the device
appearing rather than making you press Connect again.

## What is real, and what is not

**The Kit tab talks to the card.** Per-voice synth/sample assignment
(`MSG_SET_SOURCE`, applies instantly, no reboot) and WAV upload
(`MSG_UP_*`, writes flash and restarts the card) are both wired up.

**Mutes, FX and Patterns are reference displays.** The firmware has no SysEx
messages for mute-group assignment, pattern transfer or loop settings yet, so
those tabs document the card's gestures rather than configuring anything. The
page says so rather than pretending otherwise. Pattern transfer is the
obvious next one to build — a pattern is a bare event list under 2KB with no
audio attached.

## Audio conversion

Uploads are converted in the browser to the same 8-bit mono 48kHz format
`tools/importwav.py` produces for baked samples: sum to mono, resample, trim
silence (keeping 1ms of pre-roll so attacks survive), loudness-match by RMS
rather than peak, 4ms fade-out, TPDF dither. A file uploaded here should
sound like the same file baked at build time.

The loudness target (0.12) is applied across everything staged in one send,
so slots do not jump in level relative to each other, and it is set *before*
the 8-bit conversion because it decides how many of the eight bits get used.

## Two different size limits

The card reports both, and confusing them sends people deleting samples they
did not need to:

- **per-upload cap** (~160KB) is the card's RAM staging buffer. The whole
  transfer is buffered in RAM before any flash is written, because writing
  flash takes USB down with it.
- **region size** (~1MB) is storage. Uploads **append**, so audio too big for
  one pass usually fits across two.

## Provenance

The connection handling, ack queueing and upload sequence are ported from
`../../WorkshopBio/web/index.html`, whose comments record several bugs worth
not repeating — in particular the ack queue, which exists because installing
a reply handler per wait drops any reply arriving between waits and puts
every later wait one message behind.
