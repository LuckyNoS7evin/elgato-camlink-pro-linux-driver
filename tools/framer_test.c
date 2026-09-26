// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Run the driver's framer over a raw capture, feeding it in awkward piece
 * sizes to exercise chunks split across DMA segments.
 *
 *   make -C tools framer_test
 *   tools/framer_test captures/b.bin 1920 1080 [out.yuyv]
 *
 * Writes the last complete frame to out.yuyv (view with
 *   ffmpeg -f rawvideo -pix_fmt yuyv422 -s 1920x1080 -i out.yuyv out.png).
 */
#include <stdio.h>
#include <stdlib.h>

#include "../driver/clp-framer.c"

static u8 *frame;
static int handed_out;

static void *get(void *ctx, u8 **dst)
{
	(void)ctx;
	handed_out++;
	*dst = frame;
	return frame;
}

static void put(void *ctx, void *buf, bool complete)
{
	(void)buf;
	handed_out--;
	if (complete && ctx)
		*(int *)ctx = 1;
}

static const struct clp_framer_ops ops = { .get = get, .put = put };

int main(int argc, char **argv)
{
	/* segment sizes: DMA-like, odd, tiny, and header-splitting */
	static const u32 sizes[] = { 7680, 1664, 3, 4097, 7, 7680, 5, 12345 };
	struct clp_framer fr;
	u32 width, height, off = 0, i = 0;
	int have_frame = 0;
	long size;
	u8 *data;
	FILE *f;

	if (argc < 4) {
		fprintf(stderr, "usage: %s dump.bin width height [out.yuyv]\n", argv[0]);
		return 2;
	}
	width = atoi(argv[2]);
	height = atoi(argv[3]);

	f = fopen(argv[1], "rb");
	if (!f) {
		perror(argv[1]);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	rewind(f);
	data = malloc(size);
	frame = calloc(width * 2, height);
	if (!data || !frame || fread(data, 1, size, f) != (size_t)size)
		return 1;
	fclose(f);

	clp_framer_init(&fr, width, height, 0, &ops, &have_frame);
	while (off < size) {
		u32 n = sizes[i++ % (sizeof(sizes) / sizeof(sizes[0]))];

		if (n > size - off)
			n = size - off;
		clp_framer_feed(&fr, data + off, n);
		off += n;
	}
	clp_framer_flush(&fr);

	printf("chunks %llu frames %llu torn %llu dropped %llu resyncs %llu bad headers %llu, buffers outstanding %d\n",
	       (unsigned long long)fr.chunks, (unsigned long long)fr.frames,
	       (unsigned long long)fr.torn_frames, (unsigned long long)fr.dropped,
	       (unsigned long long)fr.resyncs, (unsigned long long)fr.bad_headers,
	       handed_out);

	if (argc > 4 && have_frame) {
		f = fopen(argv[4], "wb");
		if (!f || fwrite(frame, width * 2, height, f) != height)
			return 1;
		fclose(f);
	}
	return fr.frames ? 0 : 1;
}
