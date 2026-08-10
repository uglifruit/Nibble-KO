// fx.h — the twelve performance effects of FX bank 1.
//
// Reached by holding any SINGLE and tapping another: four shifts times three
// taps is twelve ordered gestures, exactly as DRUMS gets its twelve voices
// from the same four buttons (see levels.h's Shift()). Effects are MOMENTARY:
// active while the tap is held, gone the instant it is released.
//
// Two families, and they hook into completely different places:
//
//   AUDIO effects process the drum bus, one sample at a time. They live in
//   FxRack::Process(), called from the audio tick.
//
//   TIMING effects change WHEN the loop's hits fire, or add extra ones. They
//   cannot be done in the audio path at all — they act on the looper's output
//   before a voice is even triggered. See FxRack::TimingMode().
//
// Nothing here touches the recorded pattern. An effect is a filter on the way
// out, so releasing the button leaves the loop exactly as it was — which is
// what makes it safe to lean on one mid-performance.

#pragma once
#include <stdint.h>
#include "nibbleko.h"
#include "fastmath.h"

namespace nko {

/// The twelve effects, in (shift, tap) order.
///
/// The layout is chosen so the four shifts are each a FAMILY, which is what
/// makes twelve gestures learnable — you remember "B is the destructive
/// ones", not twelve arbitrary pairings.
///
///   A = filters      B = destruction     C = time      D = dynamics
enum class Fx : uint8_t {
	None = 0,

	// --- A: filters, swept by the Main knob -----------------------------
	LowPass,      ///< A+B  low-pass, Main sets cutoff
	HighPass,     ///< A+C  high-pass, Main sets cutoff
	BandSweep,    ///< A+D  a resonant band, Main sweeps it

	// --- B: destruction --------------------------------------------------
	Crush,        ///< B+A  bit-crush, Main sets depth
	Decimate,     ///< B+C  sample-rate reduction
	Fold,         ///< B+D  wavefolder, Main sets drive

	// --- C: time ---------------------------------------------------------
	Stutter,      ///< C+A  re-fire the last hit at a fixed division
	HalfTime,     ///< C+B  the loop plays at half speed
	DoubleTime,   ///< C+D  the loop plays at double speed

	// --- D: dynamics ------------------------------------------------------
	Gate,         ///< D+A  chop the bus on and off rhythmically
	Flam,         ///< D+B  every hit doubles, a few ms apart
	Silence,      ///< D+C  hard mute, for drop-outs

	kNumFx
};

/// (shift, tap) -> effect. -1 on the diagonal: a button cannot shift itself.
///
/// Read this as four rows of three. Same shape as drums.cpp's kGestureVoice
/// and for the same reason: it is the ONLY place the mapping lives, so it can
/// be rearranged without touching either the gesture reader or the DSP.
extern const Fx kFxForGesture[kNumSingles][kNumSingles];

/// What an effect does to the loop's timing, if anything.
enum class FxTiming : uint8_t {
	Normal = 0,
	Half,        ///< half speed
	Double,      ///< double speed
	Stutter,     ///< repeat the last hit at a division
	Flam,        ///< double every hit, closely spaced
};

/// Is this effect an audio-path one, or a timing one?
static inline bool IsTimingFx(Fx f)
{
	return f == Fx::Stutter || f == Fx::HalfTime
	    || f == Fx::DoubleTime || f == Fx::Flam;
}

/// How many effects can be layered at once. One per SHIFT button — each shift
/// owns an automation lane, so up to four recorded effects can overlap.
constexpr int kNumFxSlots = kNumSingles;

/// Shift C's slot holds the TIME family, and those cannot stack — half-time
/// and double-time together is a contradiction, not a texture. That slot is
/// therefore exclusive and never joins the audio chain; its effects act on
/// the looper instead. Mirrors kLaneTiming in looper.h, which is deliberately
/// NOT included here: fx.h is included BY the looper's caller, and pulling
/// looper.h in would make the dependency circular for one constant.
constexpr int kFxTimingSlot = kC;

/// One effect and the state it needs, independent of the others.
///
/// Each slot carries its OWN filter/decimator/gate state. Sharing one set
/// across the chain would make two layered filters interfere — the second
/// would read the first's charge as its own — so the cost of four copies
/// buys correctness, not just convenience. It is about 30 bytes each.
struct FxSlot
{
	Fx      fx    = Fx::None;
	int32_t depth = 2048;

	// Filter state. The three A-row effects are the same SVF read at
	// different outputs, so one set covers all three.
	int32_t v1 = 0, v2 = 0, g = 8000;

	// Decimate: the held sample and its countdown.
	int32_t holdVal = 0;
	int32_t holdCnt = 0;

	// Gate: free-running, so the chop stays rhythmic however late it is
	// grabbed rather than restarting on the press.
	uint32_t gatePhase = 0;

	/// One sample through this slot alone.
	int32_t Step(int32_t in);

	/// Reset the carried state. Called when the slot changes effect, so the
	/// new one does not open with a click of the old one's residue.
	void Clear() { v1 = v2 = 0; holdVal = holdCnt = 0; }
};

/// The effect chain: up to four slots processed in SERIES.
///
/// Series, not parallel, and not "one wins". Crush on one layer and gate on
/// another gives a crushed AND gated bus, which is the reason for layering at
/// all. Slots are walked in shift order, so the chain is deterministic —
/// A then B then D — rather than depending on the order they were recorded.
///
/// Shift C's slot is the TIMING one and never appears in the audio chain: its
/// effects act on the looper instead. See kLaneTiming in looper.h.
class FxRack
{
public:
	/// Set one slot's effect and depth. Control rate.
	/// `slot` is the shift index, 0..3. `depth` is 0..4095.
	void SetSlot(int slot, Fx f, int32_t depth);

	/// Clear every slot. Cheaper than four calls and says what it means.
	void Clear();

	/// How the loop should be re-timed, if at all. Only the timing slot can
	/// ask for this, so there is never a conflict to resolve.
	FxTiming Timing() const;

	/// True if any slot is doing anything, for the audio path to skip the
	/// whole chain when nothing is active.
	bool Any() const { return active_ != 0; }

	/// One audio sample through the whole chain.
	int32_t Step(int32_t in);

private:
	FxSlot   slot_[kNumFxSlots];
	uint8_t  active_ = 0;      ///< bit per slot, for a quick "anything on?"
};

} // namespace nko
