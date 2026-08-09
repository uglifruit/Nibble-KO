# web/

Empty on purpose. This will hold the browser sample-manager UI (WebMIDI
SysEx client), the same role as `WorkshopBio/web/index.html` — pick a synth
or sample per voice slot, upload a WAV, manage the 12-voice kit from a
browser with no toolchain.

Not written yet because it would have no protocol to speak: `webui.cpp`'s
SysEx handling is a stub until `drums.h`'s sample playback backend and the
`MSG_SET_SOURCE` protocol are designed — see `CLAUDE.md` and
`docs/LESSONS.md`. Writing the ~1200 lines of client code before that exists
would just be dead weight to carry through the design session.

`WorkshopBio/web/index.html` is the reference to build from once the
protocol is settled — its upload flow, slot picker and progress UI carry
over; only the mode×variant grid needs collapsing to a flat 12-slot list,
same adaptation already made in `samplestore.h` and `webui.h`.
