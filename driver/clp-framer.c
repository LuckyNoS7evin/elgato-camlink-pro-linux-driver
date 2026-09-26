// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Reassemble the Cam Link Pro chunk stream into frames. Payload bytes are
 * copied straight into the destination buffer; chunks may be split across
 * any number of DMA segments.
 */
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "clp-framer.h"

static const u8 clp_magic[4] = { 0xff, 0xff, 0xff, 0x00 };

void clp_framer_init(struct clp_framer *fr, u32 width, u32 height, u32 input,
		     const struct clp_framer_ops *ops, void *ctx)
{
	memset(fr, 0, sizeof(*fr));
	fr->width = width;
	fr->height = height;
	fr->input = input;
	fr->line_bytes = width * 2;
	fr->chunk = CLP_CHUNK_HDR + fr->line_bytes + CLP_CHUNK_PAD;
	fr->ops = ops;
	fr->ctx = ctx;
	fr->expect = 1;
	/* A fresh pipeline GO starts exactly on a chunk boundary. */
	fr->synced = true;
}

static void finish_frame(struct clp_framer *fr, bool complete)
{
	if (!fr->buf)
		return;
	if (complete)
		fr->frames++;
	else
		fr->torn_frames++;
	fr->ops->put(fr->ctx, fr->buf, complete);
	fr->buf = NULL;
	fr->dst = NULL;
}

/* Header just completed: validate it and decide where the payload goes. */
static bool start_chunk(struct clp_framer *fr)
{
	u32 field = fr->hdr[4] | fr->hdr[5] << 8;
	u32 width = fr->hdr[6] | fr->hdr[7] << 8;
	u32 line = field >> 3;

	if (memcmp(fr->hdr, clp_magic, sizeof(clp_magic)) || (field & 4) ||
	    width != fr->width || line < 1 || line > fr->height) {
		fr->bad_headers++;
		return false;
	}
	fr->chunks++;

	if ((field & 3) != fr->input) {
		fr->line = 0;
		return true;
	}

	if (line == 1) {
		finish_frame(fr, false);	/* previous frame never finished */
		fr->buf = fr->ops->get(fr->ctx, &fr->dst);
		if (!fr->buf)
			fr->dropped++;
		fr->torn = false;
	} else if (line != fr->expect) {
		fr->torn = true;
	}
	fr->line = line;
	fr->expect = line + 1;
	return true;
}

static void end_line(struct clp_framer *fr)
{
	if (fr->line == fr->height)
		finish_frame(fr, !fr->torn);
}

void clp_framer_feed(struct clp_framer *fr, const u8 *p, u32 len)
{
	while (len) {
		u32 n;

		if (!fr->synced) {
			/* Scan for the magic; one split across a segment
			 * boundary is missed, costing one more line. */
			u32 i;

			for (i = 0; i + sizeof(clp_magic) <= len; i++)
				if (!memcmp(p + i, clp_magic, sizeof(clp_magic)))
					break;
			if (i + sizeof(clp_magic) > len)
				return;
			p += i;
			len -= i;
			fr->synced = true;
			fr->pos = 0;
			fr->resyncs++;
			fr->torn = true;
		}

		if (fr->pos < CLP_CHUNK_HDR) {
			n = CLP_CHUNK_HDR - fr->pos;
			if (n > len)
				n = len;
			memcpy(fr->hdr + fr->pos, p, n);
			fr->pos += n;
			p += n;
			len -= n;
			if (fr->pos == CLP_CHUNK_HDR && !start_chunk(fr)) {
				fr->synced = false;
				fr->pos = 0;
			}
			continue;
		}

		if (fr->pos < CLP_CHUNK_HDR + fr->line_bytes) {
			u32 off = fr->pos - CLP_CHUNK_HDR;

			n = fr->line_bytes - off;
			if (n > len)
				n = len;
			if (fr->line && fr->dst)
				memcpy(fr->dst + (fr->line - 1) * fr->line_bytes + off,
				       p, n);
			if (off + n == fr->line_bytes && fr->line)
				end_line(fr);
		} else {
			n = fr->chunk - fr->pos;	/* pad */
			if (n > len)
				n = len;
		}
		fr->pos += n;
		p += n;
		len -= n;
		if (fr->pos == fr->chunk)
			fr->pos = 0;
	}
}

/* Stream stopped: give back a half-filled buffer. */
void clp_framer_flush(struct clp_framer *fr)
{
	finish_frame(fr, false);
}
