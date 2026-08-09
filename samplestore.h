// samplestore.h — where each voice's sample comes from.
//
// Two sources, in priority order:
//
//   1. USER FLASH  — a fixed-address region, written over USB by the WebUI.
//                    Overrides per VOICE SLOT, so you can replace just the
//                    kick and keep everything else.
//   2. BAKED       — compiled into the firmware from samples/*.raw. The
//                    factory default, so a fresh card works with no upload.
//
// The user region sits at a FIXED offset rather than after the code, so
// reflashing the firmware does not move or wipe it.
//
// Ported from WorkshopBio/samplestore.h, with the mode x round-robin-variant
// grid collapsed to 12 flat voice slots — NIBBLE-KO has no modes and (for now,
// see drums.h's VoiceSource TODO) no round robin, just one sample per slot.
//
// TODO(design session): ResolveSample() below is scaffolding — it is not
// called from anywhere yet, because DrumVoice has no PCM playback path (see
// drums.h). Wire it in once that exists. The flash layout constants
// (kUserRegionOff etc.) are a starting point, not measured against this
// card's actual code+synth-table size; checksize.cmake will catch it if
// they overlap once the real build exists.

#pragma once
#include <stdint.h>
#include "drums.h"            // kNumVoices
#include "samples_default.h"  // baked fallback

namespace nko {

// --- Flash layout ----------------------------------------------------------
// RP2040 XIP window. The user region is the last 1MB of a 2MB part — same
// split WorkshopBio uses. Revisit once this card's actual flash usage
// (synth tables + whatever baked samples ship) is known.
constexpr uint32_t kFlashBase     = 0x10000000u;
constexpr uint32_t kFlashSize     = 2u * 1024 * 1024;
constexpr uint32_t kUserRegionOff = 1u * 1024 * 1024;   // offset within flash
constexpr uint32_t kUserRegionLen = 1u * 1024 * 1024;

/// Directory written at the start of the user region. Fixed size so it can be
/// erased and rewritten as one 4KB sector.
struct UserSampleHeader
{
	uint32_t magic;       // kUserMagic when a valid upload is present
	uint32_t version;     // layout version, for future changes
	// Byte offsets are relative to the START of the user region's data area,
	// and a size of 0 means "this slot is empty, fall back to baked".
	uint32_t offset[kNumVoices];
	uint32_t size[kNumVoices];
	uint32_t totalBytes;
	uint32_t reserved[8];
};

constexpr uint32_t kUserMagic   = 0x4B4F3101u;   // "KO1" + version nibble
constexpr uint32_t kUserVersion = 1;

// The header occupies the first flash sector of the region; audio follows.
// RP2040 flash erases a sector at a time; every erase must be sector-aligned.
constexpr uint32_t kFlashSector   = 4096;

constexpr uint32_t kUserHeaderLen = 4096;
constexpr uint32_t kUserDataOff   = kUserRegionOff + kUserHeaderLen;
constexpr uint32_t kUserDataLen   = kUserRegionLen - kUserHeaderLen;

/// Read-only view of the user region as mapped through XIP.
static inline const UserSampleHeader *UserHeader()
{
	return reinterpret_cast<const UserSampleHeader *>(kFlashBase + kUserRegionOff);
}

static inline const int8_t *UserData()
{
	return reinterpret_cast<const int8_t *>(kFlashBase + kUserDataOff);
}

static inline bool HaveUserSamples()
{
	const UserSampleHeader *h = UserHeader();
	return h->magic == kUserMagic && h->version == kUserVersion;
}

/// Where a given voice slot's audio actually lives, whichever source wins.
struct SampleRef
{
	const int8_t *data;
	uint32_t      len;
};

/// Resolve one slot. A user upload for that exact slot wins; otherwise the
/// baked recording is used.
///
/// TODO(design session): not called from anywhere yet — see file header.
static inline SampleRef ResolveSample(int voice)
{
	if (voice < 0 || voice >= kNumVoices) return { nullptr, 0 };

	if (HaveUserSamples())
	{
		const UserSampleHeader *h = UserHeader();
		uint32_t sz = h->size[voice];
		if (sz > 0 && h->offset[voice] + sz <= kUserDataLen)
			return { UserData() + h->offset[voice], sz };
	}

	if (kHaveSamples)
		return { kVoiceSample[voice], kVoiceSampleLen[voice] };

	return { nullptr, 0 };
}

/// True if a slot has a user upload overriding its baked default.
static inline bool VoiceIsUserLoaded(int voice)
{
	if (voice < 0 || voice >= kNumVoices) return false;
	if (!HaveUserSamples()) return false;
	return UserHeader()->size[voice] > 0;
}

/// True if there is anything at all to play for this slot — either source.
static inline bool VoiceHasSample(int voice)
{
	if (voice < 0 || voice >= kNumVoices) return false;
	if (VoiceIsUserLoaded(voice)) return true;
	return kHaveSamples && kVoiceSampleLen[voice] > 0;
}

} // namespace nko
