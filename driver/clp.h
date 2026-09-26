/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Elgato Cam Link Pro (PCIe, 4x HDMI) capture driver.
 *
 * The card is a YUAN "SC0710" design: a Xilinx FPGA exposing a standard
 * XDMA core on BAR1 and its own video/SPI/I2C blocks on BAR0, plus a
 * Lattice ECP5 (loaded over SPI from the host) carrying the HDMI front end,
 * and an ARM MCU on I2C that reports HDMI timing and switches inputs.
 * See docs/HARDWARE.md for where each value comes from.
 */
#ifndef CLP_H
#define CLP_H

#include <linux/pci.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-v4l2.h>

#include "clp-framer.h"

#define CLP_NAME	"camlinkpro"
#define CLP_INPUTS	4

/* ---- BAR0: FPGA design registers ---- */
#define CLP_R_ID		0x0000	/* reads 0x10ee7021 */
#define CLP_R_DATE		0x0008	/* design date, BCD */
#define CLP_R_C4		0x00c4	/* written 0x000f0000 at init (meaning unknown) */
#define CLP_R_HEIGHT		0x00c8	/* input picture height */
#define CLP_R_CC		0x00cc
#define CLP_R_PIPE		0x00d0	/* pipeline control */
#define   CLP_PIPE_GO		BIT(0)
#define   CLP_PIPE_NATIVE	BIT(7)	/* bypass horizontal half-scaler (4K) */
#define   CLP_PIPE_RESET	BIT(9)
#define   CLP_PIPE_BASE		0x4100
#define CLP_R_SCALER		0x00d8
#define CLP_R_DC		0x00dc

/* ---- BAR0: Xilinx AXI Quad SPI wired to the ECP5 slave-SPI config port ---- */
#define CLP_SPI_SRR		0x2040
#define CLP_SPI_CR		0x2060
#define   CLP_SPI_CR_RESET	0x1e6	/* master, enable, manual SS, FIFO resets, inhibit */
#define   CLP_SPI_CR_INHIBIT	0x186
#define   CLP_SPI_CR_RUN	0x086
#define CLP_SPI_SR		0x2064
#define   CLP_SPI_SR_TX_EMPTY	BIT(2)
#define CLP_SPI_DTR		0x2068
#define CLP_SPI_DRR		0x206c
#define CLP_SPI_SSR		0x2070

/* ---- BAR0: Xilinx AXI IIC instances, 0x200 apart; #0 reaches the MCU ---- */
#define CLP_IIC_COUNT		8
#define CLP_IIC(n)		(0x3000 + (n) * 0x200)
#define CLP_IIC_SOFTR		0x040
#define CLP_IIC_CR		0x100
#define   CLP_IIC_CR_EN		BIT(0)
#define   CLP_IIC_CR_TXRST	BIT(1)
#define CLP_IIC_SR		0x104
#define   CLP_IIC_SR_BB		BIT(2)	/* bus busy */
#define   CLP_IIC_SR_RX_EMPTY	BIT(6)
#define   CLP_IIC_SR_TX_EMPTY	BIT(7)
#define CLP_IIC_TX		0x108
#define   CLP_IIC_TX_START	BIT(8)
#define   CLP_IIC_TX_STOP	BIT(9)
#define CLP_IIC_RX		0x10c
#define CLP_IIC_RX_PIRQ		0x120
#define CLP_IIC_TSUSTA		0x128	/* timing block runs to 0x144 */

#define CLP_MCU_ADDR		(0x32 << 1)
#define CLP_MCU_STATUS(i)	((i) * 0x3c)	/* per-input status block */
#define CLP_MCU_STATUS_LEN	0x14
#define CLP_MCU_PATH(i)		(0x3b + (i))	/* input path select */
#define   CLP_MCU_PATH_ACTIVE	0x10
#define   CLP_MCU_PATH_IDLE	0x01

/* ---- BAR1: Xilinx XDMA (PG195) ---- */
#define XDMA_C2H(n)		(0x1000 + (n) * 0x100)
#define XDMA_CH_ID		0x00
#define XDMA_CH_CTRL		0x04
#define XDMA_CH_CTRL_W1S	0x08
#define XDMA_CH_CTRL_W1C	0x0c
#define   XDMA_CTRL_RUN		BIT(0)
#define   XDMA_CTRL_RESET	BIT(24)
#define   XDMA_CTRL_IE_ALL	0x00fffe7e
#define XDMA_CH_STATUS		0x40
#define   XDMA_STATUS_BUSY	BIT(0)
#define XDMA_CH_COMPLETED	0x48
#define XDMA_CH_IE_W1S		0x94
#define XDMA_SGDMA(ch)		((ch) + 0x4000)	/* SGDMA block for a channel base */
#define XDMA_SG_ADDR_LO		0x80
#define XDMA_SG_ADDR_HI		0x84
#define XDMA_SG_ADJ		0x88
#define XDMA_SG_CREDITS		0x8c
#define XDMA_IRQ_CH_EN_W1S	0x2014
#define XDMA_IRQ_CH_EN_W1C	0x2018
#define XDMA_SG_CREDIT_EN_W1C	0x6028

struct clp_xdma_desc {
	__le32 control;		/* 0xad4b0000 | next_adj << 8 | flags */
	__le32 len;
	__le32 src_lo, src_hi;	/* C2H stream: writeback address */
	__le32 dst_lo, dst_hi;
	__le32 next_lo, next_hi;
} __packed;

#define XDMA_DESC_MAGIC		0xad4b0000
#define XDMA_WB_WORDS		8	/* 32-byte writeback slot per descriptor */
#define CLP_RING_MAX_BLOCKS	64

/* Circular card-to-host ring: descriptors never change while it runs. */
struct clp_ring {
	u32 ndesc;
	u32 seg;		/* bytes per descriptor */
	u32 segs_per_block;
	u32 nblocks;
	struct clp_xdma_desc *desc;
	dma_addr_t desc_dma;
	u32 *wb;		/* XDMA_WB_WORDS u32 per descriptor */
	dma_addr_t wb_dma;
	struct {
		void *cpu;
		dma_addr_t dma;
	} block[CLP_RING_MAX_BLOCKS];
	u64 consumed;
	u64 overruns;
};

/* Raw capture of the ring's byte stream, for reverse engineering. */
struct clp_capture {
	u8 *data;
	size_t size;
	size_t len;
	u32 *seglog;		/* per segment: writeback word 0, word 1 */
	u32 seglog_max;		/* entries (pairs) */
	u32 seglog_len;
	bool active;
	int result;
	unsigned long started;
};

struct clp_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct clp_dev {
	struct pci_dev *pdev;
	void __iomem *bar0;
	void __iomem *bar1;

	struct mutex iic_lock;
	struct mutex stream_lock;	/* ring, capture, pipeline state */

	u32 fpga_idcode;
	u32 fpga_status;

	struct clp_ring ring;
	struct clp_capture cap;
	struct task_struct *dma_thread;
	wait_queue_head_t cap_wq;
	bool streaming;
	u32 stream_input;
	u32 stream_height;

	/* V4L2 node; dev is freed by the v4l2_device release callback */
	struct v4l2_device v4l2;
	struct video_device vdev;
	struct vb2_queue queue;
	struct mutex vlock;
	spinlock_t qlock;
	struct list_head queued;
	struct clp_framer framer;
	bool video_active;		/* ring data goes to the framer */
	u32 width, height, htotal, vtotal;
	u32 sequence;
	u64 last_frame_ns;
	u32 frame_interval_us;

	struct dentry *dbg;
};

struct clp_hdmi_status {
	bool locked;
	u16 width, height;
	u16 htotal, vtotal;
	u8 raw[CLP_MCU_STATUS_LEN];
};

static inline u32 clp_rd0(struct clp_dev *dev, u32 reg)
{
	return ioread32(dev->bar0 + reg);
}

static inline void clp_wr0(struct clp_dev *dev, u32 reg, u32 val)
{
	iowrite32(val, dev->bar0 + reg);
}

static inline u32 clp_rd1(struct clp_dev *dev, u32 reg)
{
	return ioread32(dev->bar1 + reg);
}

static inline void clp_wr1(struct clp_dev *dev, u32 reg, u32 val)
{
	iowrite32(val, dev->bar1 + reg);
}

/* clp-fpga.c */
int clp_fpga_init(struct clp_dev *dev);

/* clp-mcu.c */
void clp_iic_init(struct clp_dev *dev);
int clp_mcu_read(struct clp_dev *dev, u8 sub, u8 *buf, int len);
int clp_mcu_write(struct clp_dev *dev, u8 sub, const u8 *data, int len);
int clp_mcu_hdmi_status(struct clp_dev *dev, int input, struct clp_hdmi_status *st);
int clp_mcu_set_path(struct clp_dev *dev, int input, bool active);

/* clp-dma.c */
void clp_xdma_init(struct clp_dev *dev);
void clp_ring_free(struct clp_dev *dev);
int clp_capture_run(struct clp_dev *dev, size_t bytes);
int clp_stream_start(struct clp_dev *dev, u32 input, u32 height);
void clp_stream_stop(struct clp_dev *dev);

/* clp-video.c */
int clp_video_register(struct clp_dev *dev);
void clp_video_unregister(struct clp_dev *dev);

/* clp-debugfs.c */
void clp_debugfs_init(struct clp_dev *dev);
void clp_debugfs_exit(struct clp_dev *dev);

/* module parameters shared between files */
extern unsigned int clp_input;
extern unsigned int clp_pipe_ctrl;
extern unsigned int clp_pipe_height;
extern unsigned int clp_ring_desc;
extern unsigned int clp_ring_seg;

#endif /* CLP_H */
