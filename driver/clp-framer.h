/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Chunk stream -> frames. Plain C with no kernel dependencies, so
 * tools/framer_test.c can run it against raw captures.
 *
 * Wire format (docs/HARDWARE.md): one chunk per video line,
 *   ff ff ff 00 | le16 line << 3 | input | le16 width | width * 2 bytes YUYV | 8-byte pad
 * with lines 1..height in order.
 */
#ifndef CLP_FRAMER_H
#define CLP_FRAMER_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

#define CLP_CHUNK_HDR	8
#define CLP_CHUNK_PAD	8

struct clp_framer_ops {
	/* A buffer for a new frame; NULL drops the frame. */
	void *(*get)(void *ctx, u8 **dst);
	/* Hand a buffer back: @complete is false for torn/short frames. */
	void (*put)(void *ctx, void *buf, bool complete);
};

struct clp_framer {
	/* configuration */
	u32 width, height, input;
	u32 line_bytes;
	u32 chunk;
	const struct clp_framer_ops *ops;
	void *ctx;

	/* state */
	bool synced;
	u32 pos;		/* byte offset inside the current chunk */
	u8 hdr[CLP_CHUNK_HDR];
	u32 line;		/* 1-based line of the current chunk, 0 = skip it */
	u32 expect;		/* next line we expect for this input */
	void *buf;
	u8 *dst;
	bool torn;

	/* statistics */
	u64 chunks, frames, torn_frames, dropped, resyncs, bad_headers;
};

void clp_framer_init(struct clp_framer *fr, u32 width, u32 height, u32 input,
		     const struct clp_framer_ops *ops, void *ctx);
void clp_framer_feed(struct clp_framer *fr, const u8 *p, u32 len);
void clp_framer_flush(struct clp_framer *fr);

#endif /* CLP_FRAMER_H */
