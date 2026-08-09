// main.cpp — NIBBLE-KO. Card entry point, mode machine, UI, output routing.
//
// A program card for the Music Thing Modular Workshop System Computer, built
// on Chris Johnson's header-only ComputerCard library.
//
// NIBBLE-KO reads ONE output of the Workshop System's FOUR VOLTAGES module on
// CV In 1, learns which voltage each button combination produces, and turns
// the combinations into drum hits. See levels.h for the ghost rule, which is
// the piece of this card that cannot be guessed from the code around it.
//
// ---------------------------------------------------------------------------
// THE CONTROL SURFACE
// ---------------------------------------------------------------------------
//
// The switch is a MODE SELECTOR, not a trigger. Its three positions:
//
//   DOWN    choose a mode. The four buttons pick one; the choice is committed
//           when the switch is RELEASED, and then LATCHES.
//   MIDDLE  play the latched mode.
//   UP      play the latched mode AND record it into the loop.
//
// Latching is what makes every mode recordable through one mechanism: mode
// selection is a transient Down-gesture that has finished by the time you
// reach Up, so Down and Up never need to be held at once.
//
//   switch+A  DRUMS   the kit (power-on default)
//   switch+B  MUTE    three mute-group toggles
//   switch+C  FX1     audio effects
//   switch+D  FX2     timing effects
//
//   switch+AB  enter the USB/browser setup
//   switch+AC  UNDO           switch+AD  QUANTISE
//   switch+CD  PLAY/STOP
//
// WHY SINGLES COMMIT ON RELEASE BUT PAIRS FIRE IMMEDIATELY. This asymmetry
// looks like an inconsistency and is not — both halves fall out of the ghost
// rule, pointing in opposite directions:
//
//   Four Voltages does not return to a rest voltage when you let go, so the
//   CV sits whereever it was last put. If it is ALREADY on A's level and you
//   press A to select DRUMS, the level never changes and no Trigger is ever
//   produced — a press-driven select would simply never fire, and the card
//   would look broken. Reading the STATE on release works regardless.
//
//   A pair cannot get stuck that way. Releasing a pair falls back onto one of
//   its own members, and levels.cpp sets current_ to that SINGLE (the
//   GhostArmed branch). So the resting state is always a single, a pair press
//   is always a genuine transition, and acting on the Trigger is safe.
//
// ---------------------------------------------------------------------------
// PANEL
// ---------------------------------------------------------------------------
//
//   CV In 1      Four Voltages output          (the instrument itself)
//   Pulse In 1   external clock, one edge per beat
//
//   Main   DJ filter (LP | bypass | HP)   /  FX depth in an effects mode
//   X      tempo, 40-240bpm (unless clocked)
//   Y      kit character
//
//   Audio Out 1  drum bus                 Audio Out 2  the same drum bus
//   Pulse Out 1  gate on every hit        Pulse Out 2  click, one per beat
//   CV Out 1/2   unassigned for now
//
// Calibration is meant to be the ALT-BOOT mode — hold the switch down at
// power-on — with the learned levels saved to flash and reloaded on every
// normal boot. THAT IS NOT WHAT THIS BUILD DOES: the flash side does not
// exist yet, so EVERY boot calibrates and then exits into DRUMS. See the
// splash handler in ProcessSample(), and TODO(calibstore) in LearnTick().
//
// ---------------------------------------------------------------------------
// SINGLE CORE, FOR NOW
// ---------------------------------------------------------------------------
//
// There is no multicore_launch_core1() here. The whole per-sample load — the
// drum voices, one SVF, the CV stage — is a few hundred cycles against a
// budget of 4000 at 192MHz. WorkshopBio needed a second core only because
// TinyUSB's tud_task() measured ~36000 cycles. THAT CHANGES when the WebUI
// lands: see webui.h. Until then, do not cargo-cult the multicore setup.

#include "ComputerCard.h"

#include "nibbleko.h"
#include "levels.h"
#include "drums.h"
#include "looper.h"
#include "fastmath.h"

#include "hardware/vreg.h"
#include "pico/stdlib.h"

using namespace nko;

namespace {

// ---------------------------------------------------------------------------
// Learn-mode constants
// ---------------------------------------------------------------------------

/// Ergonomic learn order: singles, then rows, columns, diagonals.
///
/// Deliberately NOT index order. The hand alternates shape instead of
/// re-forming similar grips, and on the 2x2 LED block it draws a geometric
/// sequence — top row, bottom row, left column, right column, then the two
/// diagonals — which the player reads off the panel without counting.
constexpr uint8_t kLearnOrder[kNumLevels] = { kA, kB, kC, kD,
                                              kAB, kCD, kAC, kBD, kAD, kBC };

/// Abandon a learn that has stalled. Generous, because the learn is self-paced
/// — a player may well be re-reading the panel. Its only job is to stop a card
/// being left permanently in learn.
constexpr int32_t kLearnTimeoutTicks = 30 * kCtrlRate;

constexpr int32_t kCaptureFlashTicks   = kCtrlRate / 5;    // 200ms
constexpr int32_t kCollisionFlashTicks = kCtrlRate / 2;    // 500ms, ~3 blinks
constexpr int32_t kNotSettledTicks     = kCtrlRate / 3;    // 330ms
constexpr int32_t kDoneFlashTicks      = (kCtrlRate * 2) / 5;
constexpr int32_t kFailFlashTicks      = 3 * kCtrlRate / 2;   // 1.5s

/// A learn is REJECTED outright if the ten captured levels do not span at
/// least this much of the input range.
///
/// The case this exists for is calibrating with nothing patched into CV In 1:
/// every step captures the same floating value, the card cheerfully builds a
/// table of ten identical levels, and then plays one note forever. That looks
/// like a working calibration and a broken card.
///
/// 400 units is about 1.2V. Ten levels genuinely spread across a Four Voltages
/// output cover several volts, so this rejects only the degenerate cases.
constexpr int32_t kMinLearnSpan = 400;

/// Gate length for hit events, in samples. 5ms is long enough for anything
/// downstream to see it and short enough not to smear fast playing.
constexpr int32_t kGateSamples = kSampleRate / 200;

/// Click-track pulse length. Shorter than a gate — it is a metronome tick, not
/// a note, and at 240bpm the beats are only a quarter of a second apart.
constexpr int32_t kClickSamples = kSampleRate / 500;   // 2ms

/// How long a mode-select gesture must have the switch down before the release
/// counts as a deliberate selection.
///
/// Without this, the smallest knock against the switch would re-latch the mode
/// to whatever the CV happens to be sitting on. Short enough to feel
/// instantaneous; long enough that only an intended press qualifies.
constexpr int32_t kSelectMinTicks = kCtrlRate / 20;    // 50ms

// ---------------------------------------------------------------------------

enum class UiMode : uint8_t { Play, Learn };

/// What the learn machine is doing between captures.
enum class LearnPhase : uint8_t {
	Waiting, Confirm, Collision, NotSettled, Done, Failed, Aborted
};

/// One automated knob: recorded playback, and a live hand that overrides it.
///
/// The rule: MOVING the knob mutes that lane's playback, for as long as you
/// keep moving and for a short hold afterwards. Stop, and the recorded sweep
/// takes over again from wherever it has got to.
///
/// That makes the knob a performance override rather than a mode: grab it,
/// ride the filter through a section, let go, and the pattern carries on
/// exactly as recorded. Nothing is destroyed by touching it.
///
/// What this replaces: a design that "handed control back" on a move and then
/// handed it forward again on the next recorded event, so control alternated
/// between hand and playback every few ticks whenever both were active.
/// Muting for a held window instead means only ONE of them is ever driving.
struct AutoKnob
{
	int32_t playback_  = -1;      ///< last value playback asked for, -1 = none
	int32_t smooth_    = 0;       ///< de-dithered knob reading
	int32_t last_      = -9999;   ///< reading the move detector compares against
	int32_t holdTicks_ = 0;       ///< >0 while the hand owns the knob

	/// How long the hand keeps the knob after it stops moving. Long enough to
	/// bridge the gaps in a slow deliberate turn; short enough that letting go
	/// feels immediate.
	static constexpr int32_t kHold = kCtrlRate / 4;   // 250ms

	bool HandOwns() const { return holdTicks_ > 0; }

	/// Control-rate update. Returns the value to use this tick.
	int32_t Update(int32_t live)
	{
		// Smoothed, because the move test is against a threshold and raw ADC
		// dither would otherwise trip it on a stationary knob.
		smooth_ = slew_exact(smooth_, live, 4);

		if (last_ < -9000) last_ = smooth_;      // first call: seed, do not move

		int32_t d = smooth_ - last_;
		if (d > kKnobMoveThresh || d < -kKnobMoveThresh)
		{
			last_      = smooth_;
			holdTicks_ = kHold;
		}
		else if (holdTicks_ > 0)
		{
			holdTicks_--;
		}

		if (HandOwns() || playback_ < 0) return smooth_;
		return playback_;
	}

	void Playback(int32_t v) { playback_ = v; }
	void Forget()            { playback_ = -1; }
};

} // namespace

// ===========================================================================

class NibbleKoCard : public ComputerCard
{
public:
	NibbleKoCard()
	{
		// NOTHING that touches hardware may happen here. This constructor runs
		// during ComputerCard's own construction, before Run() has set the
		// peripherals up, and heavy work here wedges the chip — a lesson paid
		// for in all three sibling cards. Plain field initialisation only.
		levels_.InitDefault();
	}

	virtual void __not_in_flash_func(ProcessSample)() override
	{
		// ---- Boot window ------------------------------------------------
		//
		// The switch is NOT readable straight away, and reads Down until it
		// settles: ComputerCard derives it from knobs[3], off a ~60Hz
		// smoothing filter starting at zero, and zero decodes as Down. So for
		// the first few milliseconds of EVERY boot the card reports Down
		// wherever the switch actually is.
		//
		// Latching on "Down seen at any point in the window" therefore latches
		// on every boot — both WorkshopZX and WorkshopBio shipped exactly that
		// bug. Take ONE reading once settled instead.
		if (bootPhase_ < kBootWindowSamples)
		{
			if (++bootPhase_ == kBootWindowSamples)
			{
				calibrateBoot_ = (SwitchVal() == Switch::Down);
				splash_        = kSplashSamples;
			}
			return;
		}

		// ---- Boot splash -------------------------------------------------
		if (splash_ > 0)
		{
			if (--splash_ == 0)
			{
				for (int i = 0; i < kNumLeds; i++) LedOff(i);

				// EVERY boot calibrates, for now.
				//
				// The intended design is that only the ALT-boot calibrates and
				// a normal boot restores the saved levels from flash — that is
				// the whole point of persisting them. But the flash side does
				// not exist yet (see TODO(calibstore) in LearnTick), so a
				// normal boot would come up on the evenly-spaced DEFAULT
				// spread, which is not a real calibration and cannot be played
				// reliably enough to test anything else.
				//
				// So both paths run the learn until calibstore lands. The
				// alt-boot latch is still read and still displayed on the
				// splash, so the gesture stays exercised and this becomes a
				// one-line change later rather than a re-think.
				EnterLearn();

				// The switch may still be held Down from an alt-boot, with its
				// release still to come. Swallow that: otherwise the release
				// reads as a mode-select gesture the moment the splash clears,
				// and the hold itself would abort the learn it just started.
				// abortLatched_ starts true and is only cleared by a release,
				// which is what makes the second half safe.
				selectArmed_ = false;
				downTicks_   = 0;
			}
			else
			{
				// An uncalibrated CV out cannot track 1V/oct, and the player
				// should learn that from the card rather than by blaming their
				// own learn pass. Blink the splash instead of holding it.
				bool show = CVOutsCalibrated() || ((splash_ >> 12) & 1);
				for (int i = 0; i < kNumLeds; i++)
					LedOn(i, show && (((i & 1) == 1) == calibrateBoot_));
			}
		}

		// ---- Pulse edges: LATCH here, at the full 48kHz ------------------
		//
		// PulseIn1RisingEdge() is true for exactly ONE sample. Polling it from
		// the 3kHz control tick therefore caught it only when the edge
		// happened to land on the 1-in-16 sample the tick ran on — about 6% of
		// the time, which is why NIBBLE's external clock could never lock.
		// Latch it at audio rate and let the control tick consume the flag.
		if (PulseIn1RisingEdge()) pulse1Edge_ = true;

		// ---- Control tick ------------------------------------------------
		//
		// NOTE: this runs INLINE, inside a DMA interrupt handler. The divider
		// lowers the AVERAGE load but does NOT relax the deadline: on the
		// sample where it fires, everything below must still finish within
		// this one 20.83us slot.
		if (++ctrlDiv_ >= kCtrlDiv) ctrlDiv_ = 0;
		if (ctrlDiv_ == 0)      ControlTick();
		else if (ctrlDiv_ == 8) UiTick();

		// ---- Audio rate --------------------------------------------------
		AudioTick();

		PulseOut1(gateTimer_ > 0);
		if (gateTimer_ > 0) gateTimer_--;

		PulseOut2(clickTimer_ > 0);
		if (clickTimer_ > 0) clickTimer_--;
	}

private:
	// =======================================================================
	// Control rate
	// =======================================================================

	void __not_in_flash_func(ControlTick)()
	{
		if (pulse1Edge_)
		{
			pulse1Edge_ = false;
			loop_.ClockPulse();
		}

		ReadSwitch();

		if (ui_ == UiMode::Learn) { LearnTick(); return; }

		// --- level detection ---
		int8_t idx = kComboNone;
		LevelEvent ev = levels_.Step(CVIn1(), idx);

		// While the switch is Down the buttons are choosing a mode, not
		// playing. Pairs act on the Trigger immediately (see the file header
		// for why that is safe); singles are read as STATE when the switch is
		// released, so nothing is dispatched for them here.
		if (selectArmed_)
		{
			if (ev == LevelEvent::Trigger && idx >= kNumSingles)
				FireAction(kActionForPair[idx - kNumSingles]);
			return;
		}

		if (ev == LevelEvent::Trigger) FireCombo(idx);

		PlayControl();
	}

	/// The switch, as a mode selector.
	///
	/// Down arms a selection and Middle/Up commits it — but only if the switch
	/// was down long enough to be deliberate, and only for a SINGLE. Pairs
	/// have already fired their action from ControlTick() by the time the
	/// release arrives, and must not also re-latch the mode on the way out.
	void __not_in_flash_func(ReadSwitch)()
	{
		Switch sw = SwitchVal();

		if (sw == Switch::Down)
		{
			if (!selectArmed_)
			{
				selectArmed_ = true;
				downTicks_   = 0;
				actionFired_ = false;
			}
			if (downTicks_ < kSelectMinTicks) downTicks_++;

			// Holding Down during a learn restarts it, which is the way out of
			// a calibration that is going wrong.
			//
			// abortLatched_ makes this fire ONCE per press. Without it the
			// abort re-fires on every tick the switch stays down — downTicks_
			// saturates at the threshold, so the test keeps passing — and
			// since an abort now RESTARTS the learn rather than leaving it,
			// that would loop: restart, abort, restart, for as long as a
			// finger rests on the switch.
			if (ui_ == UiMode::Learn && downTicks_ >= kSelectMinTicks
			 && !abortLatched_)
			{
				abortLatched_ = true;
				AbortLearn();
			}
			return;
		}

		abortLatched_ = false;

		if (!selectArmed_) return;
		selectArmed_ = false;

		// Too brief to be meant, or a pair already consumed this gesture.
		if (downTicks_ < kSelectMinTicks || actionFired_) return;

		// Commit whatever level the CV is SITTING ON, not a transition. This
		// is the half of the design that survives the CV already resting on
		// the button you are trying to pick — see the file header.
		int8_t cur = levels_.Current();
		if (cur >= 0 && cur < kNumSingles) SetMode(kModeForSingle[cur]);
	}

	void SetMode(Mode m)
	{
		if (m == mode_) { modeFlash_ = kModeFlashTicks; return; }
		mode_      = m;
		modeFlash_ = kModeFlashTicks;

		// Leaving a mode drops anything it was holding, so an effect cannot be
		// left stuck on by switching away mid-press.
		fxHeld_ = 0;

		// TODO(webui): entering Mode::WebUi sets WebUI::usbMode and hands the
		// card over. The USB stack is a stub, so for now this is inert.
	}

	/// One of the switch+pair actions.
	void __not_in_flash_func(FireAction)(Action a)
	{
		actionFired_ = true;
		switch (a)
		{
		case Action::PlayStop:
			playing_ = !playing_;
			break;

		case Action::Undo:
			// TODO(step 3): loop_.Undo() once the snapshot exists.
			break;

		case Action::Quantise:
			// TODO(step 3): loop_.QuantiseNow() once it exists.
			break;

		case Action::EnterWebUi:
			SetMode(Mode::WebUi);
			break;

		case Action::None:
		default:
			// A reserved pair. Deliberately silent rather than doing something
			// arbitrary — an unassigned gesture that acts is worse than one
			// that does not.
			break;
		}
		actionFlash_ = kModeFlashTicks;
	}

	/// A genuine new press from the level detector, dispatched by mode.
	void __not_in_flash_func(FireCombo)(int8_t combo)
	{
		if (combo < 0 || combo >= kNumLevels) return;

		switch (mode_)
		{
		case Mode::Drums:  DrumsPress(combo); break;
		case Mode::Mute:   MutePress(combo);  break;
		case Mode::Fx1:
		case Mode::Fx2:    FxPress(combo);    break;
		case Mode::WebUi:
		default:                              break;
		}
	}

	// --- DRUMS ----------------------------------------------------------

	void __not_in_flash_func(DrumsPress)(int8_t combo)
	{
		// A single button is a SHIFT, not a sound.
		//
		// Percussion wants repeated hits on the same instrument, and that is
		// exactly what a keyboard reading of the buttons cannot give you: to
		// play AC twice you must pass through C, and if C is itself a sound
		// then every repeat is interrupted by a spurious one. Making the four
		// singles silent turns them into bank-selects that can be HELD — hold
		// C and tap A, B or D, over and over.
		if (combo < kNumSingles) { FlashCombo(combo); return; }

		// A pair. WHICH pair is not enough — hold-A-tap-B and hold-B-tap-A
		// close the same two switches and produce an identical voltage, but
		// they are different gestures and get different sounds. The shift
		// button recovers the ordering the voltage threw away, doubling the
		// kit from six voices to twelve. See LevelTracker::Shift().
		int8_t shift = levels_.Shift();
		int8_t tap   = OtherMember(combo, shift);
		int8_t voice = VoiceForGesture(shift, tap);

		// No shift latched: the pair was reached without passing through one
		// of its own buttons — from another pair, or as the first press after
		// a calibration. Pick the voice either member would give rather than
		// going silent.
		if (voice < 0)
		{
			const uint8_t *m = kPairMembers[combo - kNumSingles];
			voice = VoiceForGesture(static_cast<int8_t>(m[0]),
			                        static_cast<int8_t>(m[1]));
		}
		if (voice < 0) return;

		TriggerVoice(voice);

		// The loop records the VOICE, not the gesture. A pattern is a list of
		// sounds — "kick", "snare" — and how each one was played is a property
		// of the performance. It also means re-arranging the gesture map later
		// cannot silently change what an existing loop plays.
		if (recording_) loop_.RecordHit(voice, 100);

		FlashCombo(combo);
	}

	/// Fire a voice, unless its mute group is silenced.
	///
	/// Muting is applied HERE, at the point of sounding, and never touches the
	/// loop's stored events — un-muting instantly restores exactly what was
	/// recorded. Destroying events to mute them would violate the same
	/// principle that keeps overdub lossless.
	void __not_in_flash_func(TriggerVoice)(int8_t voice)
	{
		if (voice < 0 || voice >= kNumVoices) return;
		if (muted_ & (1u << MuteGroupOf(voice))) return;

		drums_.TriggerVoice(voice, toneKnob_);
		gateTimer_ = kGateSamples;
	}

	/// Which mute group a voice belongs to.
	///
	/// TODO(webui): this is assigned per voice from the browser and stored in
	/// flash. Until that exists, split the kit three ways by index so the mode
	/// is testable on hardware: 0-3 low, 4-7 hats/metal, 8-11 the rest.
	static uint8_t MuteGroupOf(int8_t voice)
	{
		if (voice < 4) return 0;
		if (voice < 8) return 1;
		return 2;
	}

	// --- MUTE -----------------------------------------------------------

	void __not_in_flash_func(MutePress)(int8_t combo)
	{
		// Toggle, not momentary: a mute group stays where you put it. Only the
		// three singles A/B/C are groups; D and every pair are reserved.
		if (combo >= kNumMuteGroups) return;
		muted_ ^= static_cast<uint8_t>(1u << combo);
		FlashCombo(combo);

		// TODO(step 3): record the toggle as a kActionEvent when recording_.
	}

	// --- FX -------------------------------------------------------------

	void __not_in_flash_func(FxPress)(int8_t combo)
	{
		if (combo >= kNumFxPerBank) return;
		fxHeld_ |= static_cast<uint8_t>(1u << combo);
		FlashCombo(combo);

		// TODO(step 5/6): actually apply the effect. Bank 2 (timing:
		// flam/stutter/triplet) acts on the loop's firing; bank 1 (audio:
		// reverse/tape-stop/pitch) needs the PCM backend that drums.h still
		// lists as TODO. Until then this only lights the LED, which is enough
		// to test the gesture routing on hardware.
	}

	// --- shared control -------------------------------------------------

	void __not_in_flash_func(PlayControl)()
	{
		// Arming or releasing record re-seeds the knob references, so the
		// first sample after the transition cannot be mistaken for a move.
		bool nowRecording = (SwitchVal() == Switch::Up);
		if (nowRecording != recording_)
		{
			loop_.ArmKnobs();
			// TODO(step 3): loop_.Snapshot() on the way IN, so Undo has
			// something to revert to.
		}
		recording_ = nowRecording;

		// FX are momentary: an effect is only held while its button is. The
		// level detector reports the combo the CV is sitting on, so a released
		// button shows up as the CV leaving that level.
		if (mode_ == Mode::Fx1 || mode_ == Mode::Fx2)
		{
			int8_t cur = levels_.Current();
			fxHeld_ = (cur >= 0 && cur < kNumFxPerBank)
			        ? static_cast<uint8_t>(1u << cur) : 0;
		}

		loop_.SetTempo(KnobVal(Knob::X));

		// Each lane decides for itself whether the hand or the recording is
		// driving this tick. Moving the knob mutes that lane's playback while
		// you move it and for a moment after; letting go hands it back.
		const int32_t mainVal = filterLane_.Update(KnobVal(Knob::Main));
		const int32_t toneVal = toneLane_.Update(KnobVal(Knob::Y));

		djFilter_.SetKnob(mainVal);
		toneKnob_ = toneVal;

		// Record only what the HAND is doing. Recording the value that came
		// out of the lane would re-record playback on top of itself.
		if (recording_)
			loop_.RecordKnobs(filterLane_.HandOwns(), KnobVal(Knob::Main),
			                  toneLane_.HandOwns(),   KnobVal(Knob::Y));

		if (!playing_) return;

		if (loop_.Advance())
		{
			// Pulse Out 2 is a CLICK TRACK: one blip per crotchet, so you have
			// something to record along to. Driven from BeatEdge() rather than
			// OnBeat() — the latter is a level that stays true for the whole
			// tick, which at 40bpm is dozens of control steps and would give a
			// click that is on more than it is off.
			if (loop_.BeatEdge()) clickTimer_ = kClickSamples;

			int8_t  voices[Looper::kMaxFirePerTick];
			uint8_t vel[Looper::kMaxFirePerTick];
			int32_t knob[kNumLanes] = {};
			bool    haveKnob[kNumLanes] = {};

			int n = loop_.Fire(voices, vel, knob, haveKnob);

			// Hand the recorded values to the lanes. Whether they are actually
			// USED is the lane's decision — if the player is moving that knob,
			// this is remembered but muted until they let go.
			if (haveKnob[kLaneFilter]) filterLane_.Playback(knob[kLaneFilter]);
			if (haveKnob[kLaneTone])   toneLane_.Playback(knob[kLaneTone]);

			for (int i = 0; i < n; i++)
			{
				TriggerVoice(voices[i]);
				// A looped hit knows its VOICE, not which buttons would have
				// played it. Flash the pads that gesture uses, looked up from
				// the map, so playback lights what the performance did.
				FlashVoice(voices[i]);
			}
		}
	}

	// =======================================================================
	// Learn
	// =======================================================================

	void EnterLearn()
	{
		ui_          = UiMode::Learn;
		learnStep_   = 0;
		learnPhase_  = LearnPhase::Waiting;
		collisionsThisLearn_ = 0;
		learnTimer_  = kLearnTimeoutTicks;
		phaseTimer_  = 0;

		// Recalibrating CLEARS the loop. The levels are about to change, so
		// the combos an existing pattern refers to may not survive — and a
		// pattern that plays the wrong drums is worse than no pattern.
		loop_.Clear();
		filterLane_.Forget();
		toneLane_.Forget();
	}

	/// Throw away the captures and start the learn again.
	///
	/// This only ANNOUNCES the abort; LearnTick() calls EnterLearn() once the
	/// flash has played out. Restarting here directly would skip the flash
	/// entirely, and that flash is the only feedback that the gesture worked.
	///
	/// (It restarts rather than exiting because there is nowhere useful to
	/// exit to while every boot must calibrate — see LearnTick.)
	void AbortLearn()
	{
		if (learnPhase_ == LearnPhase::Aborted) return;
		learnPhase_ = LearnPhase::Aborted;
		phaseTimer_ = kCaptureFlashTicks * 2;
	}

	void __not_in_flash_func(LearnTick)()
	{
		// Keep the detector running so Settled()/SettledValue() are live.
		int8_t idx = kComboNone;
		LevelEvent ev = levels_.Step(CVIn1(), idx);

		if (phaseTimer_ > 0)
		{
			if (--phaseTimer_ == 0 && learnPhase_ != LearnPhase::Waiting)
			{
				// Only a SUCCESSFUL learn leaves calibration. Confirm and
				// Collision are per-step feedback and fall back to waiting.
				//
				// Failed and Aborted RESTART the learn rather than exiting,
				// which they did until the levels stopped being restorable
				// from flash. Exiting drops the card on the evenly-spaced
				// default, and that spread cannot actually be played — so
				// there is nothing useful to abort TO, and a mistimed switch
				// hold would cost a power cycle to undo. Starting over is the
				// only sensible destination while every boot must calibrate.
				//
				// TODO(calibstore): when saved levels exist, these two should
				// go back to exiting — aborting will then mean "keep the
				// calibration I already had", which is a real choice.
				if (learnPhase_ == LearnPhase::Done)
				{
					// Drop whatever combo the learn left "held", or the first
					// real press could match it and be swallowed as no-change.
					levels_.ResetHeld();
					ui_ = UiMode::Play;
				}
				else if (learnPhase_ == LearnPhase::Failed
				      || learnPhase_ == LearnPhase::Aborted)
				{
					EnterLearn();
				}
				else
				{
					learnPhase_ = LearnPhase::Waiting;
					learnTimer_ = kLearnTimeoutTicks;
				}
			}
			return;
		}

		if (--learnTimer_ <= 0) { AbortLearn(); return; }

		// A capture is confirmed by PRESSING the combination being asked for,
		// not by a separate control: the switch is the abort gesture now, so
		// it cannot also be the capture gesture.
		if (ev != LevelEvent::Trigger) return;
		if (idx != static_cast<int8_t>(kLearnOrder[learnStep_])) return;

		// A press that arrived mid-transition would capture a voltage the
		// player is not actually holding. Reject it and say so, rather than
		// silently recording a number from the middle of a slew.
		if (!levels_.Settled())
		{
			learnPhase_ = LearnPhase::NotSettled;
			phaseTimer_ = kNotSettledTicks;
			return;
		}

		captured_[kLearnOrder[learnStep_]] = levels_.SettledValue();

		// Warn if this level is too close to one already taken. The capture is
		// still KEPT: a learn that completes with a warning is far more useful
		// than one that refuses, and the player can move the Four Voltages
		// knob and run it again.
		bool collided = false;
		for (int j = 0; j < learnStep_; j++)
		{
			int32_t d = captured_[kLearnOrder[learnStep_]]
			          - captured_[kLearnOrder[j]];
			if (d < 0) d = -d;
			if (d < kCollisionMin) collided = true;
		}
		// Counted for every step INCLUDING the last, which is why this sits
		// above the branch. The final step gets no per-step flash of its own,
		// but it still has to be counted or the end-of-learn warning would
		// under-report a collision on the tenth capture.
		if (collided) collisionsThisLearn_++;

		learnStep_++;
		if (learnStep_ >= kNumLevels)
		{
			// Refuse a degenerate calibration rather than installing it. Ten
			// captures that all landed on the same voltage means nothing was
			// patched in — accepting that gives a card that looks calibrated
			// and plays one note forever.
			int32_t lo = captured_[0], hi = captured_[0];
			for (int i = 1; i < kNumLevels; i++)
			{
				if (captured_[i] < lo) lo = captured_[i];
				if (captured_[i] > hi) hi = captured_[i];
			}

			if (hi - lo < kMinLearnSpan)
			{
				learnPhase_ = LearnPhase::Failed;
				phaseTimer_ = kFailFlashTicks;
			}
			else
			{
				levels_.LearnFrom(captured_);
				// TODO(calibstore): persist to flash here, using the five-step
				// protocol in docs/LESSONS.md. This is the whole point of the
				// alt-boot design — without it the card recalibrates every
				// power-on, same as NIBBLE.
				learnPhase_ = LearnPhase::Done;
				phaseTimer_ = kDoneFlashTicks;
			}
		}
		else
		{
			learnPhase_ = collided ? LearnPhase::Collision : LearnPhase::Confirm;
			phaseTimer_ = collided ? kCollisionFlashTicks : kCaptureFlashTicks;
		}
	}

	// =======================================================================
	// Audio rate
	// =======================================================================

	void __not_in_flash_func(AudioTick)()
	{
		int32_t dry = drums_.Step();
		int32_t wet = djFilter_.Step(dry);
		int16_t out = clamp12(wet);

		// Mono bus to both outs: the kit is not panned, and a player patching
		// one output should get the whole kit rather than half of it.
		AudioOut1(out);
		AudioOut2(out);

		// CV Out 1 and 2 are unassigned. NIBBLE put a bassline on them; this
		// card deliberately does not. Left idle rather than given a job
		// nobody asked for — see the plan's open questions.
	}

	// =======================================================================
	// LEDs
	// =======================================================================

	/// Flash every LED belonging to a combo.
	///
	/// Note this masks the COMBO through ComboLedMask rather than indexing
	/// ledFlash_ by the combo directly: singles happen to map to themselves,
	/// but every pair would light the wrong pad — BC (index 7) would light
	/// LED 3, button D, which is not even in the combo.
	void __not_in_flash_func(FlashCombo)(int8_t combo)
	{
		uint8_t mask = ComboLedMask(combo);
		for (int i = 0; i < 4; i++)
			if (mask & (1u << i)) ledFlash_[i] = kCtrlRate / 8;
	}

	/// Flash the pads for a recorded VOICE, by finding a gesture that plays it.
	void __not_in_flash_func(FlashVoice)(int8_t voice)
	{
		for (int sh = 0; sh < kNumSingles; sh++)
			for (int tp = 0; tp < kNumSingles; tp++)
				if (VoiceForGesture(static_cast<int8_t>(sh),
				                    static_cast<int8_t>(tp)) == voice)
				{
					ledFlash_[sh] = kCtrlRate / 8;
					ledFlash_[tp] = kCtrlRate / 8;
					return;
				}
	}

	void __not_in_flash_func(UiTick)()
	{
		if (splash_ > 0) return;

		for (int i = 0; i < 4; i++)
			if (ledFlash_[i] > 0) ledFlash_[i]--;
		if (modeFlash_   > 0) modeFlash_--;
		if (actionFlash_ > 0) actionFlash_--;

		uiTicks_++;

		if (ui_ == UiMode::Learn) { LearnLeds(); return; }

		// --- choosing a mode ---------------------------------------------
		//
		// While the switch is down the LEDs show the PENDING selection, live,
		// so the player can see what releasing now would pick and move to
		// another button first. This is what makes commit-on-release legible
		// rather than mysterious — in particular it makes the "CV was already
		// sitting on that button" case visible.
		if (selectArmed_)
		{
			uint8_t mask = ComboLedMask(levels_.Current());
			bool    blink = ((uiTicks_ >> kBlinkFast) & 1) != 0;
			for (int i = 0; i < 4; i++)
				LedBrightness(i, (mask & (1u << i))
				                 ? (blink ? kLedFull : kLedDim) : 0);
			LedOff(4);
			LedOff(5);
			return;
		}

		// --- a mode or action was just committed --------------------------
		if (modeFlash_ > 0 || actionFlash_ > 0)
		{
			ModeFlashLeds();
			return;
		}

		// --- normal play ---------------------------------------------------
		ModeLeds();
	}

	/// The short announcement played when a mode is latched or an action fires.
	///
	/// Four modes on six single-colour LEDs with no screen means the steady
	/// display cannot also carry "which mode am I in" — LEDs 0-3 are busy
	/// showing content in every mode. So the mode is announced on the
	/// TRANSITION instead, as a distinct pattern per mode.
	void __not_in_flash_func(ModeFlashLeds)()
	{
		bool on = ((uiTicks_ >> kBlinkFast) & 1) != 0;

		if (actionFlash_ > 0)
		{
			// An action is not a mode: mark it on the two status LEDs only, so
			// it never reads as "you changed mode".
			for (int i = 0; i < 4; i++) LedOff(i);
			LedOn(4, on);
			LedOn(5, on);
			return;
		}

		switch (mode_)
		{
		case Mode::Drums:                          // all four, together
			for (int i = 0; i < 4; i++) LedOn(i, on);
			break;
		case Mode::Mute:                           // left column
			for (int i = 0; i < 4; i++) LedOn(i, on && ((i & 1) == 0));
			break;
		case Mode::Fx1:                            // right column
			for (int i = 0; i < 4; i++) LedOn(i, on && ((i & 1) == 1));
			break;
		case Mode::Fx2:                            // checkerboard
			for (int i = 0; i < 4; i++) LedOn(i, on == (i == 0 || i == 3));
			break;
		case Mode::WebUi:
		default:
			for (int i = 0; i < 4; i++) LedOff(i);
			break;
		}
		LedOff(4);
		LedOff(5);
	}

	/// The steady per-mode display.
	void __not_in_flash_func(ModeLeds)()
	{
		switch (mode_)
		{
		case Mode::Mute:
			// Dim = muted, off = audible. The three groups sit on their own
			// pads; the fourth is dark because it is not a group.
			for (int i = 0; i < 4; i++)
			{
				uint16_t b = 0;
				if (i < kNumMuteGroups && (muted_ & (1u << i))) b = kLedDim;
				if (ledFlash_[i] > 0) b = kLedFull;
				LedBrightness(i, b);
			}
			break;

		case Mode::Fx1:
		case Mode::Fx2:
			// Lit = that effect is held.
			for (int i = 0; i < 4; i++)
				LedBrightness(i, (fxHeld_ & (1u << i)) ? kLedFull : 0);
			break;

		case Mode::WebUi:
			// TODO(webui): show WebUI::stage as a binary count here.
			for (int i = 0; i < 4; i++)
				LedBrightness(i, ((uiTicks_ >> 9) & 1) ? kLedGlow : 0);
			break;

		case Mode::Drums:
		default:
		{
			// Show the SOUNDING combo, not the tracker's raw current level.
			// While a ghost is armed those differ: the CV has fallen back to
			// one of the released pair's members, so Current() reports that
			// single — but the pair is still what you can hear, and showing
			// the single made the display contradict the sound on every
			// release.
			uint8_t mask = ComboLedMask(levels_.Sounding());
			for (int i = 0; i < 4; i++)
			{
				uint16_t b = (mask & (1u << i)) ? kLedDim : 0;
				if (ledFlash_[i] > 0) b = kLedFull;
				LedBrightness(i, b);
			}
			break;
		}
		}

		// LED 4 is the beat, LED 5 is record — in every mode, so the two
		// things you need mid-performance never move.
		LedBrightness(4, (playing_ && loop_.OnBeat()) ? kLedFull : kLedGlow);
		LedOn(5, recording_);
	}

	void __not_in_flash_func(LearnLeds)()
	{
		switch (learnPhase_)
		{
		case LearnPhase::Confirm:
			for (int i = 0; i < kNumLeds; i++) LedOn(i, true);
			return;

		case LearnPhase::NotSettled:
			// "The voltage was still moving." Distinct from a collision: the
			// BUTTON leds flutter rather than the phase markers, which reads
			// as "your hand, not the card" — hold it steady and press again.
			LedOff(4);
			LedOff(5);
			for (int i = 0; i < 4; i++)
				LedOn(i, ((phaseTimer_ >> kBlinkFast) & 1) != 0);
			return;

		case LearnPhase::Collision:
			// Only the phase markers flash, so a collision reads differently
			// from a clean capture without stopping the learn.
			for (int i = 0; i < 4; i++) LedOff(i);
			LedOn(4, ((phaseTimer_ >> kBlinkSlow) & 1) != 0);
			LedOn(5, ((phaseTimer_ >> kBlinkSlow) & 1) != 0);
			return;

		case LearnPhase::Done:
		{
			// A clean learn ramps all six up and fades. A learn that completed
			// but recorded collisions ramps the same way with LEDs 4 and 5
			// flashing over the top — so the warning is delivered ONCE, here,
			// where you can act on it, instead of blinking forever afterwards.
			uint16_t b = static_cast<uint16_t>(
				kLedFull - (phaseTimer_ * kLedFull) / kDoneFlashTicks);
			for (int i = 0; i < 4; i++) LedBrightness(i, b);

			if (collisionsThisLearn_)
			{
				bool f = ((phaseTimer_ >> kBlinkSlow) & 1) != 0;
				LedBrightness(4, f ? kLedFull : 0);
				LedBrightness(5, f ? kLedFull : 0);
			}
			else
			{
				LedBrightness(4, b);
				LedBrightness(5, b);
			}
			return;
		}

		case LearnPhase::Failed:
			// Nothing usable came in — almost always nothing patched into
			// CV In 1. An urgent, unmistakably different pattern: the two
			// COLUMNS alternating, fast.
			for (int i = 0; i < kNumLeds; i++)
				LedOn(i, (((phaseTimer_ >> kBlinkFast) & 1) != 0)
				         == ((i & 1) == 0));
			return;

		case LearnPhase::Aborted:
			for (int i = 0; i < kNumLeds; i++)
				LedOn(i, ((phaseTimer_ >> kBlinkSlow) & 1) != 0);
			return;

		case LearnPhase::Waiting:
		default:
			break;
		}

		// Waiting for the player to press the combination being asked for.
		uint8_t want  = ComboLedMask(static_cast<int8_t>(kLearnOrder[learnStep_]));
		bool    blink = ((uiTicks_ >> kBlinkFast) & 1) != 0;

		for (int i = 0; i < 4; i++)
		{
			if (want & (1u << i))  LedBrightness(i, blink ? kLedFull : kLedDim);
			else if (Captured(i))  LedBrightness(i, kLedGlow);
			else                   LedOff(i);
		}

		// Phase marker: LED 4 during the four singles, LED 5 during the pairs.
		bool pairs = (learnStep_ >= kNumSingles);
		LedBrightness(4, pairs ? 0 : kLedDim);
		LedBrightness(5, pairs ? kLedDim : 0);
	}

	/// Has button `b` appeared in any combo captured so far? Used only to give
	/// already-visited buttons a dim glow during the singles phase.
	bool Captured(int b) const
	{
		for (int s = 0; s < learnStep_; s++)
			if (ComboLedMask(static_cast<int8_t>(kLearnOrder[s])) & (1u << b))
				return true;
		return false;
	}

	// =======================================================================
	// State
	// =======================================================================

	LevelTracker levels_;
	DrumKit      drums_;
	DjFilter     djFilter_;
	Looper       loop_;

	Mode     mode_       = Mode::Drums;   // the power-on default
	UiMode   ui_         = UiMode::Play;
	int32_t  bootPhase_  = 0;
	int32_t  splash_     = 0;
	int32_t  ctrlDiv_    = 0;
	uint32_t uiTicks_    = 0;
	bool     calibrateBoot_ = false;

	// mode select
	bool    selectArmed_  = false;  ///< switch is Down, a selection is pending
	bool    actionFired_  = false;  ///< a pair already consumed this gesture
	/// One abort per press. Starts TRUE so the alt-boot hold — which is still
	/// down when the first learn begins — cannot abort it on the way out.
	bool    abortLatched_ = true;
	int32_t downTicks_    = 0;
	int32_t modeFlash_   = 0;
	int32_t actionFlash_ = 0;

	// learn
	int32_t    captured_[kNumLevels] = {};
	int        learnStep_  = 0;
	LearnPhase learnPhase_ = LearnPhase::Waiting;
	int32_t    learnTimer_ = 0;
	int32_t    phaseTimer_ = 0;
	uint8_t    collisionsThisLearn_ = 0;

	// performance
	bool     playing_   = true;
	bool     recording_ = false;
	uint8_t  muted_     = 0;        ///< bit per mute group
	uint8_t  fxHeld_    = 0;        ///< bit per effect slot in the live bank
	AutoKnob filterLane_;           ///< Main knob: the DJ filter
	AutoKnob toneLane_;             ///< Y knob: kit character
	int32_t  toneKnob_  = 2048;     ///< the Y value voices are struck with

	// outputs
	int32_t gateTimer_  = 0;
	int32_t clickTimer_ = 0;

	/// Set at 48kHz when a Pulse In 1 edge arrives, consumed by the 3kHz
	/// control tick. See ProcessSample for why this cannot be polled directly.
	bool    pulse1Edge_ = false;

	int32_t ledFlash_[4] = {};
};

// ===========================================================================

int main()
{
	// Overclock before anything else. 192MHz at 1.15V is proven on this exact
	// hardware by the sibling cards; the brief settle after the voltage change
	// is what stops the PLL relock from landing on an unstable rail.
	vreg_set_voltage(VREG_VOLTAGE_1_15);
	sleep_ms(2);
	set_sys_clock_khz(192000, true);

	static NibbleKoCard card;
	card.Run();   // never returns
}
