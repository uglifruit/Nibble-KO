// samples_default.h — optional baked-in PCM, one slot per voice.
//
// samples.h is generated at build time by tools/mksamples.py from
// samples/*.raw (8-bit signed mono, 48kHz). It is NOT committed: a fresh
// clone with no samples still compiles, and every voice simply has no PCM to
// fall back on until the synth backend is used or the WebUI uploads one.
// Same __has_include trick WorkshopBio/WorkshopZX use.
//
// TODO(design session): this is scaffolding for the sample backend described
// in drums.h — not wired into DrumVoice/DrumKit yet. See that file's header.

#pragma once
#include <stdint.h>
#include "drums.h"   // kNumVoices

namespace nko {

#if defined(__has_include)
#  if __has_include("samples.h")
#    define NKO_HAVE_SAMPLES 1
#  endif
#endif

#ifdef NKO_HAVE_SAMPLES

// samples.h defines, per slot: voiceNN_pcm[] + voiceNN_pcm_len.
#include "samples.h"

constexpr bool kHaveSamples = true;

static const int8_t *const kVoiceSample[kNumVoices] = {
	reinterpret_cast<const int8_t *>(voice00_pcm),
	reinterpret_cast<const int8_t *>(voice01_pcm),
	reinterpret_cast<const int8_t *>(voice02_pcm),
	reinterpret_cast<const int8_t *>(voice03_pcm),
	reinterpret_cast<const int8_t *>(voice04_pcm),
	reinterpret_cast<const int8_t *>(voice05_pcm),
	reinterpret_cast<const int8_t *>(voice06_pcm),
	reinterpret_cast<const int8_t *>(voice07_pcm),
	reinterpret_cast<const int8_t *>(voice08_pcm),
	reinterpret_cast<const int8_t *>(voice09_pcm),
	reinterpret_cast<const int8_t *>(voice10_pcm),
	reinterpret_cast<const int8_t *>(voice11_pcm),
};
static const uint32_t kVoiceSampleLen[kNumVoices] = {
	voice00_pcm_len, voice01_pcm_len, voice02_pcm_len, voice03_pcm_len,
	voice04_pcm_len, voice05_pcm_len, voice06_pcm_len, voice07_pcm_len,
	voice08_pcm_len, voice09_pcm_len, voice10_pcm_len, voice11_pcm_len,
};

#else

constexpr bool kHaveSamples = false;

// Stubs so anything that includes this compiles unchanged; kHaveSamples is
// false, so these are never dereferenced.
static const int8_t *const kVoiceSample[kNumVoices] = {
	nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
	nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};
static const uint32_t kVoiceSampleLen[kNumVoices] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

#endif

} // namespace nko
