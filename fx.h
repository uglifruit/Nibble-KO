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

/// The audio-effect chain. One instance, holding whatever state the currently
/// active effect needs.
///
/// Only ONE effect is active at a time — the gesture is one shift plus one
/// tap, so there is nothing to combine. That keeps this a switch rather than
/// a chain, and keeps the per-sample cost to whichever branch is live.
class FxRack
{
public:
	/// Set the active effect and its depth. Control rate.
	/// `depth` is the Main knob, 0..4095.
	void Set(Fx f, int32_t depth);

	Fx Active() const { return active_; }

	/// How the loop should be re-timed, if at all.
	FxTiming Timing() const;

	/// One audio sample through whatever is active. Pass-through when None.
	int32_t Step(int32_t in);

private:
	Fx      active_ = Fx::None;
	int32_t depth_  = 2048;

	// Filter state, shared by the three A-row effects — they are the same
	// SVF read at different outputs, so one set of state covers all three.
	int32_t v1_ = 0, v2_ = 0, g_ = 8000;

	// Decimate: the held sample and its countdown.
	int32_t holdVal_ = 0;
	int32_t holdCnt_ = 0;

	// Gate: a free-running counter, so the chop is rhythmic rather than
	// dependent on when the button happened to be pressed.
	uint32_t gatePhase_ = 0;
};

} // namespace nko
