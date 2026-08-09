# samples/

Drop WAVs into `samples/incoming/` and run `python tools/importwav.py` to
produce `voice00.raw` .. `voice11.raw` here — 8-bit signed mono 48kHz, one
file per voice slot (see `tools/importwav.py`'s docstring for naming: either
`voiceNN.wav` or a drum name like `kick.wav`, `snare.wav`, `crash.wav`).

`tools/mksamples.py` then bakes whatever `.raw` files are present into
`samples.h` at build time. Neither the `.raw` files nor `samples.h` are
committed (see `../.gitignore`) — a fresh clone builds with none of this
present; see `../samples_default.h` for the fallback.

TODO(design session): baking samples in is scaffolded but nothing in
`drums.cpp` plays them back yet — see `../drums.h`'s file header.
