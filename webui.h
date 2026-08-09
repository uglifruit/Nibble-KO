// webui.h — USB-MIDI/SysEx sample upload for NIBBLE-KO.
//
// The card enumerates as a USB-MIDI device; a browser talks to it over
// WebMIDI SysEx to assign each of the 12 voice slots a synth voice or an
// uploaded sample. Drop a WAV in the browser, it arrives as 8-bit mono 48kHz
// and is written into the user flash region — no toolchain, no reflash.
//
// SysEx payloads must be 7-bit (every byte 0-127), so audio is 7-bit encoded:
// 7 source bytes become 8 septets, the 8th carrying the high bits. Same
// scheme WorkshopBio and WorkshopZX use.
//
// Adapted from WorkshopBio/webui.h. The message set is close to identical —
// upload/erase/slot-query are the same shape — with the mode+variant slot
// addressing collapsed to a single voice index (0..kNumVoices-1), and a new
// MSG_SET_SOURCE for choosing synth vs. sample per slot.
//
// TODO(design session): this header is the INTERFACE shape only. Method
// bodies in webui.cpp are stubs. Do not implement them until:
//   1. drums.h's VoiceSource::Sample / DrumVoice PCM playback exists — an
//      upload with nothing to play it back is dead code.
//   2. The exact protocol for MSG_SET_SOURCE and any round-robin question
//      (see drums.h) is decided.
//
// IMPORTANT, when the body IS written: writing flash while ComputerCard runs
// WILL HANG THE CARD unless all five steps in docs/LESSONS.md §"If you let
// users upload samples" are followed — raise-and-wait for core 0 to park,
// disable DMA_IRQ_0 + PWM_IRQ_WRAP + USBCTRL_IRQ, prime the boot2 RAM copy,
// wrap writes in __not_in_flash_func, and buffer the whole upload in RAM
// first. WorkshopBio/webui.cpp is the worked reference for all five.

#pragma once
#include <stdint.h>
#include "nibbleko.h"
#include "samplestore.h"

namespace nko {

// SysEx message IDs (first payload byte). fw = firmware, ui = browser.
enum : uint8_t {
	MSG_HELLO       = 0x01,  // ui->fw: announce presence; fw replies MSG_INFO
	MSG_INFO        = 0x02,  // fw->ui: version, capacity, what is loaded
	MSG_UP_BEGIN    = 0x10,  // ui->fw: start upload session (stops audio; appends)
	MSG_UP_SLOT     = 0x11,  // ui->fw: begin a voice slot; payload voice,len
	MSG_UP_CHUNK    = 0x12,  // ui->fw: 7-bit-encoded audio for the current slot
	MSG_UP_SLOTEND  = 0x13,  // ui->fw: this slot is complete
	MSG_UP_END      = 0x14,  // ui->fw: whole upload done -> commit header, resume
	MSG_UP_ACK      = 0x15,  // fw->ui: ready for more / progress
	MSG_UP_ERR      = 0x16,  // fw->ui: something went wrong; payload = code
	MSG_UP_ALIAS    = 0x17,  // ui->fw: point a slot at audio already sent
	MSG_UP_DROP     = 0x18,  // ui->fw: empty a slot, staged (no reboot)
	MSG_UP_BAKED    = 0x19,  // ui->fw: point a slot at the BUILT-IN sample
	MSG_UP_KEEP     = 0x1A,  // ui->fw: keep audio already in the user region
	MSG_ERASE       = 0x20,  // ui->fw: forget user samples, revert to baked
	MSG_PLAY        = 0x21,  // ui->fw: leave USB mode and reboot into playing
	MSG_CLEARSLOT   = 0x22,  // ui->fw: revert ONE slot to its baked sample
	MSG_SLOTS       = 0x23,  // fw->ui: which slots currently hold user audio
	MSG_SLOTINFO    = 0x24,  // ui->fw: request detail; fw replies MSG_SLOTDET
	MSG_SLOTDET     = 0x25,  // fw->ui: per-slot offset+size
	MSG_SET_SOURCE  = 0x26,  // ui->fw: set a slot to synth or sample
	                          // TODO(design session): payload shape undecided
	                          // — depends on drums.h's VoiceSource design.
	MSG_PROF_GET    = 0x30,  // ui->fw: send the timing peaks (profile builds)
	MSG_PROF        = 0x31,  // fw->ui: peak cycles per bucket + overrun count
};

// Manufacturer ID 0x7D = "prototyping / private use". Same as the sibling cards.
constexpr uint8_t kManufacturerId = 0x7D;

/// Largest single upload, set by the RAM staging buffer rather than by the
/// user flash region. Writing flash takes USB down with it, so a transfer
/// has to be received in FULL before any of it is committed.
///
/// TODO(design session): sized from WorkshopBio's figure as a placeholder.
/// Re-derive against this card's actual RAM budget (no second core / no
/// TinyUSB-driven core split has been decided either way yet).
constexpr uint32_t kUploadMax = 160u * 1024u;

// Error codes carried by MSG_UP_ERR.
enum : uint8_t {
	ERR_NONE = 0, ERR_TOO_BIG = 1, ERR_BAD_SLOT = 2, ERR_PROTOCOL = 3,
};

/// 7-bit encode/decode. Returns bytes written.
/// Encoded length = ceil(srcLen/7)*8; decoded length <= (srcLen/8)*7.
uint32_t Encode7bit(const uint8_t *src, uint32_t srcLen, uint8_t *dst, uint32_t dstMax);
uint32_t Decode7bit(const uint8_t *src, uint32_t srcLen, uint8_t *dst, uint32_t dstMax);

/// USB-MIDI transport + upload state machine.
///
/// TODO(design session): method bodies are stubs in webui.cpp — see file
/// header. Shape mirrors WorkshopBio::WebUI closely enough that porting the
/// implementation should be mechanical once the PCM backend exists; the
/// mode*variant loops there become a single loop over kNumVoices here.
class WebUI
{
public:
	void Init();
	/// Poll USB. Cheap when idle; call from the audio callback.
	void Task();

	/// True while an upload is in progress — the card mutes and stops running
	/// the kit, because flash writes stall execution anyway.
	bool Uploading() const { return uploading_; }

	/// Set by core 0 when the momentary switch has been held long enough to
	/// hand the card over to USB. Core 1 (if this card ends up needing one —
	/// see nibbleko.h and NIBBLE's "why there is no core 1" reasoning, which
	/// stops applying the moment USB is added) watches it, initialises
	/// TinyUSB the first time it goes true, and then services USB forever.
	static volatile bool usbMode;

	/// True once an upload has begun. One-way latch: the card is an upload
	/// appliance until it restarts.
	static volatile bool uploadMode;

	/// Set by core 0 once it has stopped inside its RAM-resident handler and
	/// is spinning. The flash-writing side must wait for this before touching
	/// flash — see the file header's five-step protocol.
	static volatile bool core0Parked;

	/// How far the upload got, shown on the LEDs as a binary count while
	/// uploadMode is set. See WorkshopBio/webui.h for the stage numbering this
	/// is expected to mirror.
	static volatile uint8_t stage;

	/// 0..kQ16One, for the LED progress display.
	int32_t Progress() const;

private:
	void HandleSysex(const uint8_t *msg, uint32_t len);
	void Send(const uint8_t *payload, uint32_t len);
	void SendAck(uint8_t code, uint32_t value);
	void SendErr(uint8_t code);
	void FlushPage();          // commit the 256-byte staging page to flash
	void CommitHeader();
	void WriteStagedBuffer();  // erase+program the RAM buffer, then the header

	bool     uploading_ = false;
	uint8_t  slotVoice_ = 0;
	uint32_t writeOff_ = 0;     // next byte offset within the data area
	uint32_t expected_ = 0;     // total bytes announced for this session

	// Flash writes must be whole 256-byte pages, so bytes are staged here.
	uint8_t  page_[256];
	uint32_t pageFill_ = 0;
	uint32_t pageAddr_ = 0;

	// The whole upload is staged in RAM before ANY flash is touched — see the
	// file header. Sized as a placeholder; re-derive against actual RAM use.
	uint8_t  buf_[kUploadMax];
	uint32_t bufLen_ = 0;
	uint32_t baseOff_ = 0;      // append point from the existing header
	uint32_t slotStart_ = 0;    // where the current slot began in buf_
	uint32_t slotLen_ = 0;
	bool     touched_[kNumVoices] = {};

	// The header being built up as slots arrive; written last so a
	// half-finished upload never leaves a valid-looking directory behind.
	UserSampleHeader hdr_;

	uint8_t  rx_[1024];
	uint32_t rxLen_ = 0;
	bool     inSysex_ = false;
	bool     txPending_ = false;
};

} // namespace nko
