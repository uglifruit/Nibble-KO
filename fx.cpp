// fx.cpp — the twelve performance effects.

#include "fx.h"
#include "pico.h"

namespace nko {

// Four rows of three, one family per shift. The diagonal is None because a
// button cannot shift itself.
const Fx kFxForGesture[kNumSingles][kNumSingles] = {
	//          tap A            tap B            tap C            tap D
	/* A */ { Fx::None,       Fx::LowPass,     Fx::HighPass,    Fx::BandSweep  },
	/* B */ { Fx::Crush,      Fx::None,        Fx::Decimate,    Fx::Fold       },
	/* C */ { Fx::Stutter,    Fx::HalfTime,    Fx::None,        Fx::DoubleTime },
	/* D */ { Fx::Gate,       Fx::Flam,        Fx::Silence,     Fx::None       },
};

void FxRack::Set(Fx f, int32_t depth)
{
	// Clearing the state on a CHANGE, not every call. Filters carry charge and
	// the decimator holds a sample; leaving those alive across a switch makes
	// the new effect open with a click of the old one's residue.
	if (f != active_)
	{
		v1_ = v2_ = 0;
		holdVal_ = holdCnt_ = 0;
		active_ = f;
	}
	depth_ = depth;

	// The filter coefficient, for the three effects that use one. A perceptual
	// taper — a linear sweep spends most of its travel where the ear hears
	// almost no change. Cubic-weighted, same shape as the DJ filter's.
	int32_t r = (depth_ << 15) >> 12;              // 0..32767
	int32_t q = (r * r) >> 15;
	int32_t c = (q * r) >> 15;
	g_ = 400 + ((r * 3000 + q * 8000 + c * 15000) >> 15);
	if (g_ < 200)   g_ = 200;
	if (g_ > 20000) g_ = 20000;
}

FxTiming FxRack::Timing() const
{
	switch (active_)
	{
	case Fx::HalfTime:   return FxTiming::Half;
	case Fx::DoubleTime: return FxTiming::Double;
	case Fx::Stutter:    return FxTiming::Stutter;
	case Fx::Flam:       return FxTiming::Flam;
	default:             return FxTiming::Normal;
	}
}

int32_t __not_in_flash_func(FxRack::Step)(int32_t in)
{
	switch (active_)
	{
	case Fx::None:
	default:
		return in;

	// --- A: filters ------------------------------------------------------
	//
	// One Chamberlin SVF read at three different outputs. The update order is
	// the corrected one from the DJ filter: high-pass from the PREVIOUS
	// states, integrate, then low-pass from the freshly updated band-pass.
	case Fx::LowPass:
	case Fx::HighPass:
	case Fx::BandSweep:
	{
		constexpr int32_t kRes = 14000;            // Q15, a mild emphasis
		int32_t hp = in - ((kRes * v1_) >> 15) - v2_;
		v1_ += (g_ * hp) >> 15;
		int32_t lp = v2_ + ((g_ * v1_) >> 15);
		v2_ = lp;

		// Clamp the states. An integer SVF at high g with resonance wraps
		// rather than merely getting loud, and a wrap is a full-scale square
		// wave, not a filter sound.
		constexpr int32_t kMax = 1 << 20;
		if (v1_ >  kMax) v1_ =  kMax;
		if (v1_ < -kMax) v1_ = -kMax;
		if (v2_ >  kMax) v2_ =  kMax;
		if (v2_ < -kMax) v2_ = -kMax;

		if (active_ == Fx::LowPass)  return lp;
		if (active_ == Fx::HighPass) return hp;
		return v1_ >> 2;                           // band-pass, tamed
	}

	// --- B: destruction --------------------------------------------------
	case Fx::Crush:
	{
		// Quantise to fewer bits. The knob picks how many are thrown away:
		// 1 bit at the bottom (barely audible) to 9 at the top (brutal).
		int32_t bits = 1 + ((depth_ * 8) >> 12);
		int32_t mask = ~((1 << bits) - 1);
		return in & mask;
	}

	case Fx::Decimate:
	{
		// Sample-and-hold at a reduced rate. Holding for N samples divides the
		// effective rate by N, which aliases everything above the new Nyquist
		// back down — the characteristic grainy pitch-shimmer.
		int32_t hold = 1 + ((depth_ * 32) >> 12);
		if (--holdCnt_ <= 0) { holdCnt_ = hold; holdVal_ = in; }
		return holdVal_;
	}

	case Fx::Fold:
	{
		// A wavefolder. Drive the signal up, then REFLECT anything past the
		// rails back down rather than clipping it — clipping removes harmonics
		// and folding adds them, which is why this sounds like an effect and a
		// clipper sounds like a fault.
		int32_t drive = 256 + ((depth_ * 1792) >> 12);     // Q8, 1x..8x
		int32_t v = (in * drive) >> 8;
		constexpr int32_t kLim = 2047;
		// Two folds is enough for anything reachable at 8x; a while loop here
		// would be unbounded work inside an interrupt.
		for (int i = 0; i < 3; i++)
		{
			if (v >  kLim) v =  2 * kLim - v;
			else if (v < -kLim) v = -2 * kLim - v;
			else break;
		}
		return v;
	}

	// --- D: dynamics -----------------------------------------------------
	case Fx::Gate:
	{
		// A rhythmic chop. Free-running rather than started on the press, so
		// the gate stays in phase with the music however late you grab it.
		//
		// The rate is a division of the sample clock rather than of the loop,
		// which keeps it independent of tempo — a gate that slowed with the
		// pattern would just sound like tremolo.
		uint32_t rate = 64 + static_cast<uint32_t>((4095 - depth_) * 12);
		gatePhase_ += 1;
		if (gatePhase_ >= rate) gatePhase_ = 0;
		// A short ramp at each edge, or the chop clicks on every transition.
		constexpr uint32_t kEdge = 32;
		uint32_t half = rate >> 1;
		if (gatePhase_ >= half) return 0;
		int32_t gain = 256;
		if (gatePhase_ < kEdge)          gain = static_cast<int32_t>(gatePhase_ * 8);
		else if (half - gatePhase_ < kEdge) gain = static_cast<int32_t>((half - gatePhase_) * 8);
		if (gain > 256) gain = 256;
		return (in * gain) >> 8;
	}

	case Fx::Silence:
		// A hard mute for drop-outs. Deliberately abrupt — the point is the
		// hole, and a fade would blunt it.
		return 0;

	// Timing effects do not touch the audio path at all.
	case Fx::Stutter:
	case Fx::HalfTime:
	case Fx::DoubleTime:
	case Fx::Flam:
		return in;
	}
}

} // namespace nko
