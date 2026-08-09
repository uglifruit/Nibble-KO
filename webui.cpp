// webui.cpp — USB-MIDI SysEx sample upload.
//
// TODO(design session): this is a stub. Only the 7-bit codec is implemented
// (it is pure data transform with no hardware dependency, ported unchanged
// from WorkshopBio/webui.cpp and safe to use as-is). Everything else —
// Init/Task/HandleSysex and the whole flash-write path — is a placeholder
// until drums.h's PCM backend exists and the MSG_SET_SOURCE protocol is
// decided. See webui.h's file header for what "everything else" means and
// why: writing flash while ComputerCard runs WILL HANG THE CARD unless the
// five-step protocol in docs/LESSONS.md is followed exactly.
//
// WorkshopBio/webui.cpp is the complete worked reference for that protocol
// (core-park handshake, three interrupts disabled, boot2 RAM priming,
// __not_in_flash_func writes, whole-upload-buffered-first). Port it
// mechanically once there is a PCM playback path for uploads to feed.

#include "webui.h"
#include <string.h>

namespace nko {

volatile bool WebUI::usbMode = false;
volatile bool WebUI::uploadMode = false;
volatile bool WebUI::core0Parked = false;
volatile uint8_t WebUI::stage = 0;

// ---------------------------------------------------------------------------
// 7-bit encoding — real, not a stub. Pure data transform, no hardware
// dependency, ported unchanged from WorkshopBio/webui.cpp.
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
// Everything below is a placeholder. See file header.
// ---------------------------------------------------------------------------

void WebUI::Init()
{
	// TODO(design session): TinyUSB device init, once this card actually
	// carries USB. NIBBLE (the synth-only sibling) deliberately has none —
	// see docs/LESSONS.md "Why there is no core 1". Adding uploads reopens
	// that question.
}

void WebUI::Task()
{
	// TODO(design session): poll tud_task(), dispatch completed SysEx to
	// HandleSysex(). See WorkshopBio/webui.cpp's Task() for the shape.
}

int32_t WebUI::Progress() const
{
	return 0;
}

void WebUI::HandleSysex(const uint8_t * /*msg*/, uint32_t /*len*/)
{
	// TODO(design session): dispatch on the message IDs in webui.h. The
	// mode*variant loops in WorkshopBio's version collapse to a single loop
	// over kNumVoices here — otherwise the message set carries over closely.
}

void WebUI::Send(const uint8_t * /*payload*/, uint32_t /*len*/)
{
	// TODO(design session)
}

void WebUI::SendAck(uint8_t /*code*/, uint32_t /*value*/)
{
	// TODO(design session)
}

void WebUI::SendErr(uint8_t /*code*/)
{
	// TODO(design session)
}

void WebUI::FlushPage()
{
	// TODO(design session): commit page_[] to flash. MUST follow the
	// five-step protocol in webui.h's file header — see
	// WorkshopBio/webui.cpp's FlushPage()/WriteStagedBuffer() for the worked
	// version (__not_in_flash_func, interrupts saved, core 0 parked first).
}

void WebUI::CommitHeader()
{
	// TODO(design session)
}

void WebUI::WriteStagedBuffer()
{
	// TODO(design session): the five-step protocol lives here. Do not write
	// this without re-reading docs/LESSONS.md's upload section first — it is
	// the one place on this platform where skipping a step hangs the chip
	// rather than merely producing a wrong answer.
}

} // namespace nko
