// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Card-to-host video DMA: one free-running circular ring on XDMA C2H
 * channel 0, serviced by polling the per-descriptor writebacks in host
 * memory (no MMIO in the hot path).
 *
 * The service loop hands the byte stream to the framer (V4L2 streaming)
 * and/or copies it raw into a debugfs capture buffer (tools/analyze_ring.py).
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/kthread.h>
#include <linux/sizes.h>
#include <linux/vmalloc.h>

#include "clp.h"

#define VCH		XDMA_C2H(0)
#define VSG		XDMA_SGDMA(VCH)
#define DESC_PER_PAGE	(PAGE_SIZE / sizeof(struct clp_xdma_desc))

/* Leave this many completed slots unread before calling it an overrun. */
#define RING_OVERRUN_MARGIN	32
#define SEGLOG_OVERRUN		0xffffffff

/* Interrupt-enable masks as the vendor driver programs them. */
void clp_xdma_init(struct clp_dev *dev)
{
	static const struct { u32 reg, val; } init[] = {
		{ 0x0094, 0x00fffe3e }, { 0x0008, 0x00fffe3e },	/* H2C 0 */
		{ 0x0194, 0x00fffe3e }, { 0x0108, 0x00fffe3e },	/* H2C 1 */
		{ 0x1094, 0x00fffe7e }, { 0x1008, 0x00fffe7e },	/* C2H 0 */
		{ 0x1194, 0x00fffe7e }, { 0x1108, 0x00fffe7e },	/* C2H 1 */
		{ 0x2080, 0 }, { 0x2084, 0 }, { 0x2088, 0 }, { 0x208c, 0 },
		{ 0x20a0, 0 }, { 0x20a4, 0 },			/* vector maps */
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(init); i++)
		clp_wr1(dev, init[i].reg, init[i].val);
}

void clp_ring_free(struct clp_dev *dev)
{
	struct clp_ring *r = &dev->ring;
	struct device *d = &dev->pdev->dev;
	u32 i;

	for (i = 0; i < r->nblocks; i++)
		dma_free_coherent(d, (size_t)r->segs_per_block * r->seg,
				  r->block[i].cpu, r->block[i].dma);
	if (r->desc)
		dma_free_coherent(d, r->ndesc * sizeof(*r->desc),
				  r->desc, r->desc_dma);
	if (r->wb)
		dma_free_coherent(d, r->ndesc * XDMA_WB_WORDS * 4,
				  r->wb, r->wb_dma);
	memset(r, 0, sizeof(*r));
}

static void *ring_seg_cpu(struct clp_ring *r, u32 idx)
{
	return r->block[idx / r->segs_per_block].cpu +
	       (size_t)(idx % r->segs_per_block) * r->seg;
}

static int ring_alloc(struct clp_dev *dev)
{
	struct clp_ring *r = &dev->ring;
	struct device *d = &dev->pdev->dev;
	u32 seg = clamp_t(u32, clp_ring_seg & ~0xfu, 0x200, SZ_64K);
	u32 ndesc = clamp_t(u32, clp_ring_desc, 16, 4096);
	u32 i;

	if (r->desc && r->seg == seg && r->ndesc == ndesc)
		return 0;
	clp_ring_free(dev);

	/* Payload in ~1 MiB coherent blocks, never one huge allocation. */
	r->segs_per_block = min_t(u32, max_t(u32, 1, SZ_1M / seg), ndesc);
	if (DIV_ROUND_UP(ndesc, r->segs_per_block) > CLP_RING_MAX_BLOCKS)
		ndesc = r->segs_per_block * CLP_RING_MAX_BLOCKS;
	r->seg = seg;
	r->ndesc = ndesc;

	r->desc = dma_alloc_coherent(d, ndesc * sizeof(*r->desc),
				     &r->desc_dma, GFP_KERNEL);
	r->wb = dma_alloc_coherent(d, ndesc * XDMA_WB_WORDS * 4,
				   &r->wb_dma, GFP_KERNEL);
	if (!r->desc || !r->wb)
		goto nomem;

	for (i = 0; i < DIV_ROUND_UP(ndesc, r->segs_per_block); i++) {
		r->block[i].cpu = dma_alloc_coherent(d,
				(size_t)r->segs_per_block * seg,
				&r->block[i].dma, GFP_KERNEL);
		if (!r->block[i].cpu)
			goto nomem;
		r->nblocks++;
	}

	for (i = 0; i < ndesc; i++) {
		struct clp_xdma_desc *desc = &r->desc[i];
		u32 next = (i + 1) % ndesc;
		dma_addr_t buf = r->block[i / r->segs_per_block].dma +
				 (dma_addr_t)(i % r->segs_per_block) * seg;
		dma_addr_t wb = r->wb_dma + (dma_addr_t)i * XDMA_WB_WORDS * 4;
		dma_addr_t nd = r->desc_dma + (dma_addr_t)next * sizeof(*desc);
		/* Descriptors that follow "next" contiguously in its page,
		 * so the engine can fetch them in one burst. */
		u32 adj = min_t(u32, 63, DESC_PER_PAGE - 1 - next % DESC_PER_PAGE);

		adj = min_t(u32, adj, ndesc - 1 - next);
		desc->control = cpu_to_le32(XDMA_DESC_MAGIC | adj << 8);
		desc->len = cpu_to_le32(seg);
		desc->src_lo = cpu_to_le32(lower_32_bits(wb));
		desc->src_hi = cpu_to_le32(upper_32_bits(wb));
		desc->dst_lo = cpu_to_le32(lower_32_bits(buf));
		desc->dst_hi = cpu_to_le32(upper_32_bits(buf));
		desc->next_lo = cpu_to_le32(lower_32_bits(nd));
		desc->next_hi = cpu_to_le32(upper_32_bits(nd));
	}

	dev_info(d, "ring: %u descriptors x %u bytes (%u KiB)\n",
		 ndesc, seg, (ndesc * seg) >> 10);
	return 0;

nomem:
	clp_ring_free(dev);
	return -ENOMEM;
}

static void xdma_prep(struct clp_dev *dev)
{
	struct clp_ring *r = &dev->ring;

	clp_wr1(dev, VCH + XDMA_CH_CTRL_W1C, XDMA_CTRL_RUN);
	clp_wr1(dev, VCH + XDMA_CH_CTRL_W1S, XDMA_CTRL_RESET);
	mdelay(1);
	clp_wr1(dev, VCH + XDMA_CH_CTRL_W1C, XDMA_CTRL_RESET | XDMA_CTRL_RUN);
	clp_wr1(dev, VCH + XDMA_CH_CTRL_W1S, XDMA_CTRL_IE_ALL);
	clp_wr1(dev, VCH + XDMA_CH_IE_W1S, XDMA_CTRL_IE_ALL);

	memset(r->wb, 0, r->ndesc * XDMA_WB_WORDS * 4);
	r->consumed = 0;
	r->overruns = 0;
	wmb();

	clp_wr1(dev, VCH + XDMA_CH_COMPLETED, 0);
	clp_wr1(dev, VSG + XDMA_SG_ADDR_HI, upper_32_bits(r->desc_dma));
	clp_wr1(dev, VSG + XDMA_SG_ADDR_LO, lower_32_bits(r->desc_dma));
	clp_wr1(dev, VSG + XDMA_SG_ADJ,
		min_t(u32, 63, min_t(u32, DESC_PER_PAGE, r->ndesc) - 1));
}

static void xdma_run(struct clp_dev *dev)
{
	/* Free-running ring: no descriptor credit gating, no interrupts. */
	clp_wr1(dev, XDMA_SG_CREDIT_EN_W1C, BIT(16));
	clp_wr1(dev, XDMA_IRQ_CH_EN_W1C, 0xff);
	clp_wr1(dev, VCH + XDMA_CH_CTRL_W1S, XDMA_CTRL_RUN);
	/* Credits are only accepted once the engine runs. */
	clp_wr1(dev, VSG + XDMA_SG_CREDITS, dev->ring.ndesc);
}

static void xdma_stop(struct clp_dev *dev)
{
	u32 st;

	clp_wr1(dev, VCH + XDMA_CH_CTRL_W1C, XDMA_CTRL_RUN);
	if (read_poll_timeout(clp_rd1, st, !(st & XDMA_STATUS_BUSY),
			      100, 20000, false, dev, VCH + XDMA_CH_STATUS))
		dev_warn(&dev->pdev->dev, "XDMA still busy after stop\n");
}

/*
 * Pipeline sequence used by the reference driver (not traced from Windows
 * for this board, so it is one of the things under test). GO comes last,
 * after the DMA engine is already running.
 */
static void pipe_program(struct clp_dev *dev)
{
	u32 ctrl = clp_pipe_ctrl & ~CLP_PIPE_GO;

	clp_wr0(dev, CLP_R_HEIGHT, dev->stream_height);
	clp_wr0(dev, CLP_R_PIPE, ctrl);
	clp_wr0(dev, CLP_R_CC, 0);
	clp_wr0(dev, CLP_R_DC, 0);
	clp_wr0(dev, CLP_R_PIPE, ctrl | CLP_PIPE_RESET);
	clp_wr0(dev, CLP_R_PIPE, ctrl);
}

/* Returns the number of segments consumed. Runs only in the DMA thread. */
static u32 ring_service(struct clp_dev *dev)
{
	struct clp_ring *r = &dev->ring;
	struct clp_capture *cap = &dev->cap;
	u32 *ahead, n = 0;

	/* If the slot almost a full lap ahead has completed, the engine lapped
	 * us: everything in the ring is suspect. Drop it and note the gap. */
	ahead = r->wb + ((r->consumed + r->ndesc - RING_OVERRUN_MARGIN) %
			 r->ndesc) * XDMA_WB_WORDS;
	if (READ_ONCE(ahead[0])) {
		memset(r->wb, 0, r->ndesc * XDMA_WB_WORDS * 4);
		wmb();
		r->consumed += r->ndesc;
		r->overruns++;
		if (cap->active && cap->seglog_len < cap->seglog_max) {
			cap->seglog[2 * cap->seglog_len] = SEGLOG_OVERRUN;
			cap->seglog[2 * cap->seglog_len + 1] = 0;
			cap->seglog_len++;
		}
		return 0;
	}

	while (n < r->ndesc - RING_OVERRUN_MARGIN) {
		u32 idx = r->consumed % r->ndesc;
		u32 *w = r->wb + idx * XDMA_WB_WORDS;
		u32 status = READ_ONCE(w[0]);
		u32 len;

		if (!status)
			break;
		dma_rmb();
		len = min_t(u32, READ_ONCE(w[1]), r->seg);

		if (dev->video_active)
			clp_framer_feed(&dev->framer, ring_seg_cpu(r, idx), len);

		if (cap->active) {
			size_t room = cap->size - cap->len;

			memcpy(cap->data + cap->len, ring_seg_cpu(r, idx),
			       min_t(size_t, len, room));
			cap->len += min_t(size_t, len, room);
			if (cap->seglog_len < cap->seglog_max) {
				cap->seglog[2 * cap->seglog_len] = status;
				cap->seglog[2 * cap->seglog_len + 1] = w[1];
				cap->seglog_len++;
			}
			if (cap->len == cap->size) {
				cap->active = false;
				wake_up(&dev->cap_wq);
			}
		}

		w[0] = 0;
		w[1] = 0;
		r->consumed++;
		n++;
	}
	wmb();
	return n;
}

static int dma_thread_fn(void *arg)
{
	struct clp_dev *dev = arg;
	unsigned long last = jiffies;

	while (!kthread_should_stop()) {
		if (ring_service(dev))
			last = jiffies;
		else if (dev->cap.active && time_after(jiffies, last + 2 * HZ)) {
			dev->cap.result = -ETIMEDOUT;
			dev->cap.active = false;
			wake_up(&dev->cap_wq);
		}
		usleep_range(250, 500);
	}
	return 0;
}

static void log_xdma_state(struct clp_dev *dev, const char *when)
{
	dev_info(&dev->pdev->dev,
		 "%s: xdma ctrl %08x status %08x completed %u, pipe %08x, consumed %llu, overruns %llu\n",
		 when, clp_rd1(dev, VCH + XDMA_CH_CTRL),
		 clp_rd1(dev, VCH + XDMA_CH_STATUS),
		 clp_rd1(dev, VCH + XDMA_CH_COMPLETED),
		 clp_rd0(dev, CLP_R_PIPE), dev->ring.consumed,
		 dev->ring.overruns);
}

/* Caller holds stream_lock. */
int clp_stream_start(struct clp_dev *dev, u32 input, u32 height)
{
	struct device *d = &dev->pdev->dev;
	int ret;

	ret = ring_alloc(dev);
	if (ret)
		return ret;

	dev->stream_input = input;
	dev->stream_height = clp_pipe_height ? clp_pipe_height : height;

	xdma_prep(dev);

	ret = clp_mcu_set_path(dev, dev->stream_input, true);
	if (ret)
		dev_warn(d, "HDMI-%u path enable failed (%d)\n",
			 dev->stream_input + 1, ret);

	pipe_program(dev);
	xdma_run(dev);
	clp_wr0(dev, CLP_R_PIPE, clp_rd0(dev, CLP_R_PIPE) | CLP_PIPE_GO);

	dev->dma_thread = kthread_run(dma_thread_fn, dev, CLP_NAME "-dma");
	if (IS_ERR(dev->dma_thread)) {
		ret = PTR_ERR(dev->dma_thread);
		dev->dma_thread = NULL;
		dev->streaming = true;
		clp_stream_stop(dev);
		return ret;
	}
	dev->streaming = true;
	dev_info(d, "streaming HDMI-%u, height %u, pipe %08x\n",
		 dev->stream_input + 1, dev->stream_height,
		 clp_rd0(dev, CLP_R_PIPE));
	return 0;
}

void clp_stream_stop(struct clp_dev *dev)
{
	if (!dev->streaming)
		return;

	log_xdma_state(dev, "stop");
	clp_wr0(dev, CLP_R_PIPE, clp_rd0(dev, CLP_R_PIPE) & ~CLP_PIPE_GO);
	xdma_stop(dev);
	if (dev->dma_thread) {
		kthread_stop(dev->dma_thread);
		dev->dma_thread = NULL;
	}
	clp_mcu_set_path(dev, dev->stream_input, false);
	dev->streaming = false;
}

/* Stream until @bytes of raw ring data are captured; caller holds stream_lock. */
int clp_capture_run(struct clp_dev *dev, size_t bytes)
{
	struct clp_capture *cap = &dev->cap;
	struct device *d = &dev->pdev->dev;
	struct clp_hdmi_status st;
	u32 input = clp_input % CLP_INPUTS;
	long left;
	int ret;

	if (dev->streaming)
		return -EBUSY;

	vfree(cap->data);
	vfree(cap->seglog);
	memset(cap, 0, sizeof(*cap));

	cap->data = vmalloc(bytes);
	/* Generous: short segments are exactly what we want to see. */
	cap->seglog_max = bytes / 256 + 4096;
	cap->seglog = vmalloc(array_size(cap->seglog_max, 2 * sizeof(u32)));
	if (!cap->data || !cap->seglog) {
		vfree(cap->data);
		vfree(cap->seglog);
		memset(cap, 0, sizeof(*cap));
		return -ENOMEM;
	}
	cap->size = bytes;
	cap->started = jiffies;
	cap->active = true;

	if (clp_mcu_hdmi_status(dev, input, &st) || !st.locked)
		st.height = 1080;
	ret = clp_stream_start(dev, input, st.height);
	if (ret) {
		cap->active = false;
		return ret;
	}

	left = wait_event_interruptible_timeout(dev->cap_wq, !cap->active,
						30 * HZ);
	cap->active = false;
	clp_stream_stop(dev);

	if (left == 0 && !cap->result)
		cap->result = -ETIMEDOUT;
	else if (left < 0)
		cap->result = left;

	dev_info(d, "capture: %zu/%zu bytes, %u segments in %u ms, result %d\n",
		 cap->len, cap->size, cap->seglog_len,
		 jiffies_to_msecs(jiffies - cap->started), cap->result);
	return cap->result;
}
