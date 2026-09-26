// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * I2C access to the board MCU (7-bit address 0x32) through the FPGA's
 * Xilinx AXI IIC controller #0, in its "dynamic" mode: START/STOP flags
 * ride in bits 8/9 of each TX FIFO word, and a read is queued as
 * START|addr|1 followed by STOP|count.
 *
 * The MCU hides the four HDMI receivers. It keeps one status block per
 * input and a path register per input that must say "active" before the
 * FPGA gets pixels from that input.
 */
#include <linux/delay.h>
#include <linux/iopoll.h>

#include "clp.h"

#define IIC0(reg)	(CLP_IIC(0) + (reg))

static int iic_wait_sr(struct clp_dev *dev, u32 mask, u32 want)
{
	u32 sr;

	return read_poll_timeout(clp_rd0, sr, (sr & mask) == want,
				 20, 20000, false, dev, IIC0(CLP_IIC_SR));
}

static void iic_reset_one(struct clp_dev *dev, u32 base)
{
	static const u32 timing[] = {
		0x2d, 0x2d, 0x2d, 0x14, 0x50, 0x76, 0x76, 0x01,
	};	/* TSUSTA TSUSTO THDSTA TSUDAT TBUF THIGH TLOW THDDAT */
	int i;

	clp_wr0(dev, base + CLP_IIC_SOFTR, 0x0a);
	udelay(10);
	for (i = 0; i < ARRAY_SIZE(timing); i++)
		clp_wr0(dev, base + CLP_IIC_TSUSTA + 4 * i, timing[i]);
}

/* All eight controllers come up wedged until soft-reset and timed. */
void clp_iic_init(struct clp_dev *dev)
{
	int n;

	for (n = 0; n < CLP_IIC_COUNT; n++)
		iic_reset_one(dev, CLP_IIC(n));
}

static int __mcu_write(struct clp_dev *dev, const u8 *buf, int len)
{
	int i, ret;

	ret = iic_wait_sr(dev, CLP_IIC_SR_BB, 0);
	if (ret)
		return ret;

	clp_wr0(dev, IIC0(CLP_IIC_CR), CLP_IIC_CR_TXRST);
	clp_wr0(dev, IIC0(CLP_IIC_CR), CLP_IIC_CR_EN);
	clp_wr0(dev, IIC0(CLP_IIC_TX), CLP_IIC_TX_START | CLP_MCU_ADDR);

	/* One byte in flight at a time, as the vendor driver does. */
	for (i = 0; i < len; i++) {
		ret = iic_wait_sr(dev, CLP_IIC_SR_TX_EMPTY, CLP_IIC_SR_TX_EMPTY);
		if (ret)
			return ret;
		clp_wr0(dev, IIC0(CLP_IIC_TX),
			buf[i] | (i == len - 1 ? CLP_IIC_TX_STOP : 0));
	}
	ret = iic_wait_sr(dev, CLP_IIC_SR_TX_EMPTY, CLP_IIC_SR_TX_EMPTY);
	if (ret)
		return ret;
	return iic_wait_sr(dev, CLP_IIC_SR_BB, 0);
}

static int __mcu_read(struct clp_dev *dev, u8 sub, u8 *buf, int len)
{
	int i, ret;

	ret = iic_wait_sr(dev, CLP_IIC_SR_BB, 0);
	if (ret)
		return ret;

	/* Write the subaddress, leaving the bus held (no STOP)... */
	clp_wr0(dev, IIC0(CLP_IIC_CR), CLP_IIC_CR_TXRST);
	clp_wr0(dev, IIC0(CLP_IIC_CR), CLP_IIC_CR_EN);
	clp_wr0(dev, IIC0(CLP_IIC_TX), CLP_IIC_TX_START | CLP_MCU_ADDR);
	clp_wr0(dev, IIC0(CLP_IIC_TX), sub);
	ret = iic_wait_sr(dev, CLP_IIC_SR_TX_EMPTY, CLP_IIC_SR_TX_EMPTY);
	if (ret)
		return ret;

	/* ...give the MCU time to fetch it (bus traces show ~1 ms)... */
	usleep_range(1000, 1500);

	/* ...then a repeated START for the read. */
	clp_wr0(dev, IIC0(CLP_IIC_RX_PIRQ), 0x0f);
	clp_wr0(dev, IIC0(CLP_IIC_CR), CLP_IIC_CR_TXRST);
	clp_wr0(dev, IIC0(CLP_IIC_CR), 0);
	clp_wr0(dev, IIC0(CLP_IIC_TX), CLP_IIC_TX_START | CLP_MCU_ADDR | 1);
	clp_wr0(dev, IIC0(CLP_IIC_TX), CLP_IIC_TX_STOP | len);
	clp_wr0(dev, IIC0(CLP_IIC_CR), CLP_IIC_CR_EN);

	for (i = 0; i < len; i++) {
		ret = iic_wait_sr(dev, CLP_IIC_SR_RX_EMPTY, 0);
		if (ret)
			return ret;
		buf[i] = clp_rd0(dev, IIC0(CLP_IIC_RX));
	}
	return iic_wait_sr(dev, CLP_IIC_SR_BB, 0);
}

int clp_mcu_read(struct clp_dev *dev, u8 sub, u8 *buf, int len)
{
	int ret;

	if (len < 1 || len > 255)
		return -EINVAL;

	mutex_lock(&dev->iic_lock);
	ret = __mcu_read(dev, sub, buf, len);
	if (ret)	/* never leave stale bytes to offset the next read */
		iic_reset_one(dev, CLP_IIC(0));
	mutex_unlock(&dev->iic_lock);
	return ret;
}

int clp_mcu_write(struct clp_dev *dev, u8 sub, const u8 *data, int len)
{
	u8 buf[17];
	int ret;

	if (len < 0 || len > 16)
		return -EINVAL;
	buf[0] = sub;
	memcpy(buf + 1, data, len);

	mutex_lock(&dev->iic_lock);
	ret = __mcu_write(dev, buf, len + 1);
	if (ret)
		iic_reset_one(dev, CLP_IIC(0));
	mutex_unlock(&dev->iic_lock);
	return ret;
}

/*
 * Status block layout (little-endian), as far as known:
 *   0x04 vtotal  0x06 htotal  0x08 height  0x0a width
 *   0x0d colorimetry / interlace   0x0f colorspace
 */
int clp_mcu_hdmi_status(struct clp_dev *dev, int input,
			struct clp_hdmi_status *st)
{
	u8 *r = st->raw;
	int ret;

	memset(st, 0, sizeof(*st));
	ret = clp_mcu_read(dev, CLP_MCU_STATUS(input), r, CLP_MCU_STATUS_LEN);
	if (ret)
		return ret;

	st->vtotal = r[0x04] | r[0x05] << 8;
	st->htotal = r[0x06] | r[0x07] << 8;
	st->height = r[0x08] | r[0x09] << 8;
	st->width = r[0x0a] | r[0x0b] << 8;
	st->locked = st->width && st->height;
	return 0;
}

int clp_mcu_set_path(struct clp_dev *dev, int input, bool active)
{
	u8 v = active ? CLP_MCU_PATH_ACTIVE : CLP_MCU_PATH_IDLE;
	int ret;

	ret = clp_mcu_write(dev, CLP_MCU_PATH(input), &v, 1);
	usleep_range(2000, 3000);
	return ret;
}
