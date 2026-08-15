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
// grid collapsed to 12 flat voice slots — NIBBLE-KO has no modes and no round
// robin, just one sample per slot.
//
// ResolveSample() IS wired in: DrumKit::TriggerVoice calls it per hit, and a
// voice with no sample falls through to the synth engine. The baked half
// works today; the user-flash half is still a stub, so HaveUserSamples()
// always answers false until webui.cpp learns to write the region.
//
// EVERYTHING HERE IS INDIRECTION, DELIBERATELY. The full chain is:
//
//     pattern event -> voice index -> gVoiceSample -> bank index -> here
//
// and each arrow is a lookup rather than a copy. That is what lets uploads
// change what a pattern SOUNDS like without touching the pattern, which stays
// a bare list of voice indices (see looper.h's note on LoopEvent). It also
// means two voices can share a recording, and that re-pointing a slot costs
// one byte rather than a second copy of the audio.
//
// The slot->bank assignment is runtime data: gVoiceSample below, seeded from
// the baked kVoiceSample defaults and rewritten by the browser. It is not yet
// PERSISTED, so a reboot restores the defaults — UserSampleHeader::reserved[]
// is budgeted for that next revision.
//
// TODO(webui): the user region's layout is a starting point, not measured
// against this card's real code+bank size. checksize.cmake guards the
// boundary now that uploads are live.

#pragma once
#include <stdint.h>
#include "nibbleko.h"         // kFlashBase/kFlashSize/kFlashSector
#include "drums.h"            // kNumVoices
#include "samples_default.h"  // baked fallback

namespace nko {

// --- Flash layout ----------------------------------------------------------
// kFlashBase/kFlashSize/kFlashSector live in nibbleko.h, which describes the
// physical part once for every region that carves it up.
//
// The user region is the last 1MB of the 2MB part — the same split
// WorkshopBio uses.
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
constexpr uint32_t kUserHeaderLen = kFlashSector;
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

// SampleRef lives in samples_default.h, since the baked bank is the source
// that always exists; the user region is the override on top of it.

/// WHICH bank entry each voice slot plays, at runtime. -1 keeps the slot
/// synthesised.
///
/// Starts as a copy of samples_default.h's baked kVoiceSample defaults, and
/// is rewritten per-slot by the browser over MSG_SET_SOURCE (see webui.h).
/// That is the whole reason it exists: kVoiceSample is constexpr, so the
/// assignment had to become data the card can change without a reflash.
///
/// RAM ONLY for now, so a reboot restores the baked defaults — the same tier
/// as the mute-group mapping. Persisting it belongs with the next revision of
/// UserSampleHeader, which has reserved[] budgeted for exactly this.
///
/// `inline` because both drums.cpp and webui.cpp include this header and both
/// need the same array, not one each.
inline int8_t gVoiceSample[kNumVoices] = {
	kVoiceSample[0],  kVoiceSample[1],  kVoiceSample[2],  kVoiceSample[3],
	kVoiceSample[4],  kVoiceSample[5],  kVoiceSample[6],  kVoiceSample[7],
	kVoiceSample[8],  kVoiceSample[9],  kVoiceSample[10], kVoiceSample[11],
};
static_assert(kNumVoices == 12, "gVoiceSample's initialiser is written out "
                                "per slot and must match kNumVoices");

/// Point a slot at a bank entry, or at -1 to synthesise it. Bounds-checked
/// because the caller is a SysEx message from a browser, i.e. a system
/// boundary rather than trusted internal code.
static inline bool SetVoiceSample(int voice, int bank)
{
	if (voice < 0 || voice >= kNumVoices) return false;
	if (bank < -1 || bank >= kMaxSamples) return false;
	gVoiceSample[voice] = static_cast<int8_t>(bank);
	return true;
}

/// Resolve one VOICE SLOT to the audio it should play, or {nullptr, 0} for
/// "this voice is synthesised".
///
/// Two sources, in priority order:
///   1. a user upload for that slot, if one has been sent over USB;
///   2. the baked bank entry kVoiceSample maps the slot to.
///
/// Note the asymmetry: the user region is indexed by VOICE, because an upload
/// is "replace what this pad plays", while the baked side is indexed through
/// the bank, because a shipped library is shared. Both end up as a pointer
/// and a length, which is all the voice needs.
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

	return BakedSample(gVoiceSample[voice]);
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
	return BakedSample(gVoiceSample[voice]).len > 0;
}

} // namespace nko
