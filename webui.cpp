// webui.cpp — USB-MIDI SysEx sample upload.
//
// Ported from WorkshopBio/webui.cpp, which is the hardware-proven reference
// for every hazardous part of this: the core-park handshake, the three
// interrupts that must be masked, priming the boot2 RAM copy, wrapping the
// writes in __not_in_flash_func, and buffering the whole upload in RAM before
// any of it is committed. Read docs/LESSONS.md §"If you let users upload
// samples" before changing anything below the codec — this is the one place on
// this platform where skipping a step HANGS THE CHIP rather than producing a
// wrong answer.
//
// WHAT IS DIFFERENT FROM THE REFERENCE. BioMimicry addresses slots as
// mode x variant; NIBBLE-KO has twelve flat voice slots, so every doubly
// nested loop there becomes a single loop here. That also removes a whole
// class of bug the reference had to grow defences against: it clears a MODE
// when any slot in it is uploaded, so a cross-mode baked reference could make
// an untouched mode play another mode's recordings. With flat slots there is
// no mode to clear, so uploading one voice simply cannot disturb another, and
// modeCleared_/kBakedFlag have no counterpart here.
//
// The other difference is how the card gets here: BioMimicry enumerates after
// the switch is held for two seconds, NIBBLE-KO after the switch+B+D combo
// sets WebUI::usbMode (see main.cpp's Action::EnterWebUi). Same modal design —
// TinyUSB is not initialised, and the card does not appear on USB at all,
// until the player asks for it.

#include "webui.h"
#include "fastmath.h"        // kQ16One, for Progress()
#include <string.h>
#include "tusb.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/irq.h"
#include "hardware/watchdog.h"
#include "hardware/dma.h"

namespace nko {

volatile bool WebUI::usbMode = false;
volatile bool WebUI::uploadMode = false;
volatile bool WebUI::core0Parked = false;
volatile uint8_t WebUI::stage = 0;

// ---------------------------------------------------------------------------
// 7-bit encoding — pure data transform, no hardware dependency, ported
// unchanged from WorkshopBio/webui.cpp.
// ---------------------------------------------------------------------------

uint32_t Encode7bit(const uint8_t *src, uint32_t srcLen, uint8_t *dst,
                    uint32_t dstMax)
{
	uint32_t o = 0;
	for (uint32_t i = 0; i < srcLen; i += 7)
	{
		uint32_t n = (srcLen - i < 7) ? (srcLen - i) : 7;
		if (o + n + 1 > dstMax) break;
		uint8_t high = 0;
		for (uint32_t k = 0; k < n; k++)
			if (src[i + k] & 0x80) high |= static_cast<uint8_t>(1u << k);
		dst[o++] = high;
		for (uint32_t k = 0; k < n; k++)
			dst[o++] = src[i + k] & 0x7F;
	}
	return o;
}

uint32_t Decode7bit(const uint8_t *src, uint32_t srcLen, uint8_t *dst,
                    uint32_t dstMax)
{
	uint32_t o = 0;
	for (uint32_t i = 0; i < srcLen; i += 8)
	{
		uint8_t high = src[i];
		uint32_t n = (srcLen - i - 1 < 7) ? (srcLen - i - 1) : 7;
		for (uint32_t k = 0; k < n; k++)
		{
			if (o >= dstMax) return o;
			dst[o++] = static_cast<uint8_t>(src[i + 1 + k]
			         | ((high & (1u << k)) ? 0x80 : 0x00));
		}
	}
	return o;
}

// ---------------------------------------------------------------------------
// Flash writing
// ---------------------------------------------------------------------------

/// Stop the audio engine for the duration of a flash write.
///
/// Core 0 cannot survive XIP going down: ComputerCard's DMA callback dispatches
/// through a vtable in flash, so it faults before any software guard could run.
/// So core 0 parks itself inside its own RAM-resident ProcessSample() (see
/// main.cpp) and says so via core0Parked; only then is writing safe.
///
/// calibstore.h does a smaller version of this for the saved calibration and
/// its header explains why it needs less: no USB in that path. Here there is,
/// so all of it applies.
static void EnterUploadMode()
{
	if (WebUI::uploadMode) return;

	// Raise the flag FIRST, then wait for the acknowledgement. ProcessSample
	// only runs when DMA_IRQ_0 fires, so masking before the flag is seen would
	// guarantee core 0 never parks. At 48kHz the next callback is 21us away.
	WebUI::uploadMode = true;
	WebUI::stage = 1;

	absolute_time_t deadline = make_timeout_time_ms(250);
	while (!WebUI::core0Parked && !time_reached(deadline)) tight_loop_contents();

	WebUI::stage = 2;

	// Silence every flash-resident interrupt handler. DMA_IRQ_0 and
	// PWM_IRQ_WRAP are the audio path (ComputerCard runs a SECOND ISR for CV
	// out); USBCTRL_IRQ is TinyUSB's, whose handler is in flash like the rest
	// of the stack. Nothing can talk to the host after this line, which is why
	// the whole upload is buffered in RAM and only committed here.
	irq_set_enabled(DMA_IRQ_0, false);
	irq_set_enabled(PWM_IRQ_WRAP, false);
	irq_set_enabled(USBCTRL_IRQ, false);

	// Prime the SDK's boot2 RAM copy while XIP is definitely still up.
	// flash_range_erase/program restore XIP afterwards by calling a RAM copy of
	// boot2, which flash_init_boot2_copyout() populates lazily by reading FROM
	// XIP the first time any flash function runs. A zero-length erase reaches
	// that initialiser and does nothing else, so the copy is taken here rather
	// than during the first real write.
	flash_range_erase(kUserRegionOff, 0);
	WebUI::stage = 3;
}

/// Keep servicing USB for `ms` so queued replies actually reach the host.
/// tud_midi_stream_write only fills a buffer; tud_task() moves it onto the
/// wire. Anything that replies and then reboots must pump in between.
static void __not_in_flash_func(FlushUsb)(uint32_t ms)
{
	absolute_time_t until = make_timeout_time_ms(ms);
	while (!time_reached(until)) tud_task();
}

static void __not_in_flash_func(EraseRegion)(uint32_t off, uint32_t len)
{
	uint32_t ints = save_and_disable_interrupts();
	flash_range_erase(off, len);
	restore_interrupts(ints);
}

static void __not_in_flash_func(ProgramPage)(uint32_t off, const uint8_t *data)
{
	uint32_t ints = save_and_disable_interrupts();
	flash_range_program(off, data, 256);
	restore_interrupts(ints);
}

// ---------------------------------------------------------------------------

void WebUI::Init()
{
	tusb_init();
	memset(&hdr_, 0, sizeof(hdr_));
}

int32_t __not_in_flash_func(WebUI::Progress)() const
{
	if (!uploading_ || expected_ == 0) return 0;
	uint64_t p = static_cast<uint64_t>(bufLen_) * kQ16One / expected_;
	return (p > kQ16One) ? kQ16One : static_cast<int32_t>(p);
}

void __not_in_flash_func(WebUI::Task)()
{
	tud_task();

	uint8_t packet[4];
	while (tud_midi_available())
	{
		if (!tud_midi_packet_read(packet)) break;

		// Reassemble SysEx from USB-MIDI 4-byte packets. The code-index number
		// in the low nibble says how many of the three data bytes are real.
		uint8_t cin = packet[0] & 0x0F;
		uint32_t n = 0;
		switch (cin)
		{
		case 0x4: n = 3; break;                 // sysex continues
		case 0x5: n = 1; break;                 // ends with 1 byte
		case 0x6: n = 2; break;                 // ends with 2 bytes
		case 0x7: n = 3; break;                 // ends with 3 bytes
		default:  n = 0; break;
		}

		for (uint32_t i = 0; i < n; i++)
		{
			uint8_t b = packet[1 + i];
			if (b == 0xF0) { inSysex_ = true; rxLen_ = 0; continue; }
			if (b == 0xF7)
			{
				if (inSysex_) HandleSysex(rx_, rxLen_);
				inSysex_ = false;
				rxLen_ = 0;
				continue;
			}
			if (inSysex_ && rxLen_ < sizeof(rx_)) rx_[rxLen_++] = b;
		}
	}

	// Push anything queued by a reply above, OUTSIDE the packet loop where
	// re-entering tud_task() is safe. Without this an ack can sit in the FIFO
	// while the browser — which waits for one before sending the next chunk —
	// times out.
	if (txPending_)
	{
		txPending_ = false;
		tud_task();
	}
}

void __not_in_flash_func(WebUI::Send)(const uint8_t *payload, uint32_t len)
{
	uint8_t buf[64];
	if (len + 3 > sizeof(buf)) return;
	uint32_t o = 0;
	buf[o++] = 0xF0;
	buf[o++] = kManufacturerId;
	memcpy(buf + o, payload, len);
	o += len;
	buf[o++] = 0xF7;
	tud_midi_stream_write(0, buf, o);

	// Do NOT call tud_task() here to push it out. Send() is reached from inside
	// Task()'s packet loop via HandleSysex(), so a nested tud_task() re-enters
	// that loop and corrupts rx_/rxLen_ half way through parsing the very
	// message being replied to. Task() flushes on the way out instead.
	txPending_ = true;
}

void __not_in_flash_func(WebUI::SendAck)(uint8_t code, uint32_t value)
{
	uint8_t p[5] = { MSG_UP_ACK, code,
	                 static_cast<uint8_t>((value >> 14) & 0x7F),
	                 static_cast<uint8_t>((value >> 7)  & 0x7F),
	                 static_cast<uint8_t>( value        & 0x7F) };
	Send(p, 5);
}

void __not_in_flash_func(WebUI::SendErr)(uint8_t code)
{
	uint8_t p[2] = { MSG_UP_ERR, code };
	Send(p, 2);
	uploading_ = false;
}

void __not_in_flash_func(WebUI::FlushPage)()
{
	if (pageFill_ == 0) return;
	memset(page_ + pageFill_, 0, 256 - pageFill_);
	ProgramPage(pageAddr_, page_);
	pageAddr_ += 256;
	pageFill_ = 0;
}

/// Commit the RAM-staged upload to flash. Runs only after EnterUploadMode(),
/// with audio stopped and USB already dead — nothing here can report progress,
/// so WebUI::stage on the LEDs is the only visible signal.
void __not_in_flash_func(WebUI::WriteStagedBuffer)()
{
	// Where the new audio lands: appended after whatever is already stored and
	// rounded UP to a sector, so the erase cannot clip the tail of a recording
	// this upload never touched.
	uint32_t base = (baseOff_ + kFlashSector - 1u) & ~(kFlashSector - 1u);
	uint32_t dst  = kUserDataOff + base;

	uint32_t end = (dst + bufLen_ + kFlashSector - 1u) & ~(kFlashSector - 1u);
	if (end > dst) EraseRegion(dst, end - dst);
	WebUI::stage = 5;

	uint32_t off = 0;
	while (off + 256 <= bufLen_)
	{
		ProgramPage(dst + off, buf_ + off);
		off += 256;
	}
	if (off < bufLen_)
	{
		memset(page_, 0, sizeof(page_));
		memcpy(page_, buf_ + off, bufLen_ - off);
		ProgramPage(dst + off, page_);
	}
	WebUI::stage = 6;

	// Staging-buffer offsets become flash offsets now the base is known. Only
	// slots this session actually STAGED audio for get rebased — a slot pointed
	// at bytes already in flash (MSG_UP_KEEP) holds a real flash offset that
	// must be left alone.
	for (int v = 0; v < kNumVoices; v++)
		if (hdr_.size[v] > 0 && touched_[v])
			hdr_.offset[v] += base;

	writeOff_ = base + bufLen_;
	CommitHeader();
}

void __not_in_flash_func(WebUI::CommitHeader)()
{
	FlushPage();
	hdr_.magic = kUserMagic;
	hdr_.version = kUserVersion;
	hdr_.totalBytes = writeOff_;

	// The header lives in its own sector and is written LAST: until the magic
	// lands, an interrupted upload falls back to baked samples rather than
	// pointing at half-written audio.
	static uint8_t sector[kUserHeaderLen];
	memset(sector, 0xFF, sizeof(sector));
	memcpy(sector, &hdr_, sizeof(hdr_));
	EraseRegion(kUserRegionOff, kUserHeaderLen);
	for (uint32_t i = 0; i < kUserHeaderLen; i += 256)
		ProgramPage(kUserRegionOff + i, sector + i);
}

void __not_in_flash_func(WebUI::HandleSysex)(const uint8_t *msg, uint32_t len)
{
	if (len < 2 || msg[0] != kManufacturerId) return;
	const uint8_t *p = msg + 1;
	uint32_t n = len - 1;

	switch (p[0])
	{
	case MSG_HELLO:
	{
		// Version, capacity, and what is actually loaded, so the browser can
		// describe the card rather than guess.
		uint32_t used = 0;
		if (HaveUserSamples())
		{
			const UserSampleHeader *uh = UserHeader();
			used = uh->totalBytes;
			if (used > kUserDataLen) used = kUserDataLen;
		}

		uint8_t info[14] = {
			MSG_INFO, kUserVersion,
			static_cast<uint8_t>(HaveUserSamples() ? 1 : 0),
			static_cast<uint8_t>(kHaveSamples ? 1 : 0),
			// The STAGING BUFFER size, not the flash region: one upload is
			// limited by the RAM it is buffered in. The browser must reject
			// oversized files against this, not against the 1MB region.
			static_cast<uint8_t>((kUploadMax >> 14) & 0x7F),
			static_cast<uint8_t>((kUploadMax >> 7)  & 0x7F),
			static_cast<uint8_t>( kUploadMax        & 0x7F),
			static_cast<uint8_t>(kNumVoices),
			// How much of the region is spoken for, and how big it is. Uploads
			// APPEND, so these are what tell the browser whether the card is
			// genuinely full or merely cannot take one huge file in one pass.
			static_cast<uint8_t>((used >> 14) & 0x7F),
			static_cast<uint8_t>((used >> 7)  & 0x7F),
			static_cast<uint8_t>( used        & 0x7F),
			static_cast<uint8_t>((kUserDataLen >> 14) & 0x7F),
			static_cast<uint8_t>((kUserDataLen >> 7)  & 0x7F),
			static_cast<uint8_t>( kUserDataLen        & 0x7F) };
		Send(info, 14);
		break;
	}

	case MSG_UP_BEGIN:
	{
		// payload: 3 septets of total byte count
		if (n < 4) { SendErr(ERR_PROTOCOL); break; }
		expected_ = (static_cast<uint32_t>(p[1]) << 14)
		          | (static_cast<uint32_t>(p[2]) << 7)
		          |  static_cast<uint32_t>(p[3]);
		if (expected_ > kUserDataLen) { SendErr(ERR_TOO_BIG); break; }
		if (expected_ > sizeof(buf_)) { SendErr(ERR_TOO_BIG); break; }

		// The card keeps playing through the whole transfer. Flash is not
		// touched until MSG_UP_END, because writing it means killing USB —
		// TinyUSB's stack and its ISR are all in flash, so the card cannot
		// receive the next chunk and write the last one at the same time.
		uploading_ = true;
		pageFill_ = 0;
		bufLen_ = 0;

		// Start from what is already on the card rather than a blank table, so
		// uploading one voice leaves the other eleven alone.
		const UserSampleHeader *cur = UserHeader();
		if (cur->magic == kUserMagic && cur->version == kUserVersion)
		{
			memcpy(&hdr_, cur, sizeof(hdr_));
			baseOff_ = cur->totalBytes;
		}
		else
		{
			memset(&hdr_, 0, sizeof(hdr_));
			baseOff_ = 0;
		}
		memset(touched_, 0, sizeof(touched_));

		if (baseOff_ + expected_ > kUserDataLen) { SendErr(ERR_TOO_BIG); break; }

		WebUI::stage = 4;
		SendAck(0, 0);
		break;
	}

	case MSG_UP_SLOT:
	{
		// payload: voice index. No mode to clear first — slots are flat here,
		// so beginning a slot cannot disturb any other one.
		if (!uploading_ || n < 2) { SendErr(ERR_PROTOCOL); break; }
		slotVoice_ = p[1];
		if (slotVoice_ >= kNumVoices) { SendErr(ERR_BAD_SLOT); break; }

		// Each slot starts page-aligned in the staging buffer so it can still
		// be programmed independently when the whole lot is committed.
		bufLen_ = (bufLen_ + 255u) & ~255u;
		slotStart_ = bufLen_;
		slotLen_ = 0;
		SendAck(1, bufLen_);
		break;
	}

	case MSG_UP_CHUNK:
	{
		if (!uploading_) { SendErr(ERR_PROTOCOL); break; }
		uint8_t dec[512];
		uint32_t got = Decode7bit(p + 1, n - 1, dec, sizeof(dec));
		if (bufLen_ + got > sizeof(buf_)) { SendErr(ERR_TOO_BIG); break; }

		// Straight into RAM. Flash stays untouched until UP_END.
		memcpy(buf_ + bufLen_, dec, got);
		bufLen_ += got;
		slotLen_ += got;
		WebUI::stage = 5;
		SendAck(2, bufLen_);
		break;
	}

	case MSG_UP_SLOTEND:
		if (!uploading_) { SendErr(ERR_PROTOCOL); break; }
		// Where this slot landed in the STAGING buffer; rewritten to a flash
		// offset when the buffer is committed.
		hdr_.offset[slotVoice_] = slotStart_;
		hdr_.size[slotVoice_]   = slotLen_;
		touched_[slotVoice_]    = true;
		SendAck(3, slotLen_);
		break;

	case MSG_UP_ALIAS:
	{
		// Point one voice at audio already sent in THIS session, rather than
		// sending it twice. The header is a plain (offset,size) pair per slot,
		// so nothing stops two voices naming the same bytes — mapping one
		// recording onto two pads costs the flash of one.
		if (!uploading_ || n < 3) { SendErr(ERR_PROTOCOL); break; }
		uint8_t dst = p[1], src = p[2];
		if (dst >= kNumVoices || src >= kNumVoices) { SendErr(ERR_BAD_SLOT); break; }
		if (hdr_.size[src] == 0) { SendErr(ERR_PROTOCOL); break; }
		hdr_.offset[dst] = hdr_.offset[src];
		hdr_.size[dst]   = hdr_.size[src];
		touched_[dst]    = touched_[src];
		SendAck(8, hdr_.size[dst]);
		break;
	}

	case MSG_UP_KEEP:
	{
		// Keep audio ALREADY in the user region: payload voice, then 3 septets
		// of its existing flash offset. Lets the browser re-send a partial set
		// without destroying slots it no longer holds the files for.
		if (!uploading_ || n < 5) { SendErr(ERR_PROTOCOL); break; }
		uint8_t dst = p[1];
		if (dst >= kNumVoices) { SendErr(ERR_BAD_SLOT); break; }
		uint32_t off = (static_cast<uint32_t>(p[2]) << 14)
		             | (static_cast<uint32_t>(p[3]) << 7)
		             |  static_cast<uint32_t>(p[4]);

		// Recover the size from the on-flash header: the browser knows where
		// the audio is, not how long it is.
		uint32_t sz = 0;
		const UserSampleHeader *cur = UserHeader();
		if (cur->magic == kUserMagic && cur->version == kUserVersion)
			for (int v = 0; v < kNumVoices; v++)
				if (cur->offset[v] == off && cur->size[v] > 0)
					{ sz = cur->size[v]; break; }
		if (!sz) { SendErr(ERR_PROTOCOL); break; }

		hdr_.offset[dst] = off;      // already a flash offset
		hdr_.size[dst]   = sz;
		touched_[dst]    = false;    // so the commit rebase leaves it alone
		SendAck(11, sz);
		break;
	}

	case MSG_UP_DROP:
	{
		// Empty one voice as part of the current session, staged like anything
		// else and committed at UP_END. MSG_CLEARSLOT does the same thing but
		// commits and reboots at once, which would abort a sync mid-flight.
		if (!uploading_ || n < 2) { SendErr(ERR_PROTOCOL); break; }
		uint8_t v = p[1];
		if (v >= kNumVoices) { SendErr(ERR_BAD_SLOT); break; }
		hdr_.offset[v] = 0;
		hdr_.size[v]   = 0;
		touched_[v]    = false;
		SendAck(9, 0);
		break;
	}

	case MSG_UP_END:
	{
		if (!uploading_) { SendErr(ERR_PROTOCOL); break; }
		uploading_ = false;

		// Ack and push it out BEFORE touching flash. Once the write starts USB
		// is gone, so this is the last chance to say anything. It is an "about
		// to commit" ack, not a "committed" one — the browser treats the
		// disconnect that follows as success.
		SendAck(4, bufLen_);
		FlushUsb(150);

		// Everything below runs with the card silent and USB dead.
		EnterUploadMode();
		WriteStagedBuffer();
		WebUI::stage = 7;

		watchdog_reboot(0, 0, 0);
		break;
	}

	case MSG_SET_SOURCE:
	{
		// payload: voice, bank — where bank is kSourceSynth (0x7F) for "leave
		// this slot synthesised", else an index into the sample bank.
		//
		// RAM only, deliberately: this writes gVoiceSample[], which resets to
		// the baked defaults on reboot. No flash write means no EnterUploadMode
		// and no reboot, so the browser can flip a slot and hear it instantly.
		if (n < 3) { SendErr(ERR_PROTOCOL); break; }
		int voice = p[1];
		int bank  = (p[2] == kSourceSynth) ? -1 : p[2];
		if (!SetVoiceSample(voice, bank)) { SendErr(ERR_BAD_SLOT); break; }
		SendAck(12, static_cast<uint32_t>(p[2]));
		break;
	}

	case MSG_SLOTS:
	{
		// Which voices hold USER audio, and what each voice is currently
		// pointed at. Two septets of bitmask covers twelve slots; a single byte
		// would silently drop slots 8-11.
		static_assert(kNumVoices <= 14, "two septets of bitmask covers 14 slots");
		uint8_t sl[3 + kNumVoices];
		uint32_t o = 0;
		sl[o++] = MSG_SLOTS;

		uint32_t bits = 0;
		for (int v = 0; v < kNumVoices; v++)
			if (VoiceIsUserLoaded(v)) bits |= (1u << v);
		sl[o++] = static_cast<uint8_t>(bits & 0x7F);
		sl[o++] = static_cast<uint8_t>((bits >> 7) & 0x7F);

		// The live source assignment, so the browser shows what the card will
		// actually play rather than what was baked at build time.
		for (int v = 0; v < kNumVoices; v++)
			sl[o++] = (gVoiceSample[v] < 0)
			        ? kSourceSynth
			        : static_cast<uint8_t>(gVoiceSample[v] & 0x7F);
		Send(sl, o);
		break;
	}

	case MSG_SLOTINFO:
	{
		// Per-slot offset and size, for a browser that wants to send
		// MSG_UP_KEEP for audio it is not re-uploading.
		if (n < 2) { SendErr(ERR_PROTOCOL); break; }
		uint8_t v = p[1];
		if (v >= kNumVoices) { SendErr(ERR_BAD_SLOT); break; }

		uint32_t off = 0, sz = 0;
		if (HaveUserSamples())
		{
			const UserSampleHeader *h = UserHeader();
			off = h->offset[v];
			sz  = h->size[v];
		}
		uint8_t d[8] = { MSG_SLOTDET, v,
		                 static_cast<uint8_t>((off >> 14) & 0x7F),
		                 static_cast<uint8_t>((off >> 7)  & 0x7F),
		                 static_cast<uint8_t>( off        & 0x7F),
		                 static_cast<uint8_t>((sz >> 14) & 0x7F),
		                 static_cast<uint8_t>((sz >> 7)  & 0x7F),
		                 static_cast<uint8_t>( sz        & 0x7F) };
		Send(d, 8);
		break;
	}

	case MSG_CLEARSLOT:
	{
		// Revert ONE voice to its baked sample, committed immediately. Rewrites
		// the header sector only — the audio stays but is unreferenced.
		if (n < 2) { SendErr(ERR_PROTOCOL); break; }
		uint8_t v = p[1];
		if (v >= kNumVoices) { SendErr(ERR_BAD_SLOT); break; }
		if (!HaveUserSamples()) { SendAck(6, 0); break; }

		memcpy(&hdr_, UserHeader(), sizeof(hdr_));
		hdr_.offset[v] = 0;
		hdr_.size[v]   = 0;
		writeOff_ = hdr_.totalBytes;

		SendAck(6, 0);
		FlushUsb(150);
		EnterUploadMode();
		CommitHeader();
		watchdog_reboot(0, 0, 0);
		break;
	}

	case MSG_ERASE:
		// Wipe just the header sector: the audio stays but is unreferenced, so
		// the card falls straight back to its baked samples. This is also how
		// space is reclaimed — uploads append, so re-uploading the same voice
		// repeatedly eventually fills the region, and this resets it to empty.
		SendAck(5, 0);
		FlushUsb(200);
		EnterUploadMode();
		EraseRegion(kUserRegionOff, kUserHeaderLen);
		watchdog_reboot(0, 0, 0);
		break;

	case MSG_PLAY:
		// Leave USB mode and come back up playing. The reboot is what tears
		// TinyUSB down cleanly; calibration survives it, being in flash since
		// calibstore.h landed.
		SendAck(7, 0);
		FlushUsb(200);
		watchdog_reboot(0, 0, 0);
		break;

	default:
		break;
	}
}

} // namespace nko
