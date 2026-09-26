// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Lattice ECP5 configuration over the FPGA's AXI Quad SPI master.
 *
 * The ECP5 holds its configuration in SRAM, so it comes up blank after
 * every power cycle and has to be loaded from the host before any HDMI
 * data flows. Elgato ships the bitstream as CAMLINKPRO.FWI.HEX, lightly
 * scrambled; clp_fwi_decode() undoes that.
 */
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/iopoll.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#include "clp.h"

#define CLP_FW_NAME		"sc0710/CAMLINKPRO.FWI.HEX"
MODULE_FIRMWARE(CLP_FW_NAME);

static bool fpga_reload;
module_param(fpga_reload, bool, 0644);
MODULE_PARM_DESC(fpga_reload, "Reload the ECP5 bitstream even if already configured");

#define ECP5_IDCODE_85F		0x41113043

#define ECP5_READ_ID		0xe0
#define ECP5_LSC_READ_STATUS	0x3c
#define ECP5_LSC_CHECK_BUSY	0xf0
#define ECP5_LSC_REFRESH	0x79
#define ECP5_ISC_ENABLE		0xc6
#define ECP5_ISC_ERASE		0x0e
#define ECP5_ISC_DISABLE	0x26
#define ECP5_LSC_INIT_ADDRESS	0x46
#define ECP5_LSC_BITSTREAM_BURST 0x7a

#define ECP5_STATUS_DONE	BIT(8)
#define ECP5_STATUS_ERRORS	0x03802000	/* fail flag (13), BSE error code (25:23) */
#define ECP5_BUSY		BIT(7)

/* FWI container: 16-byte header, then the bitstream stored as two halves
 * in swapped order, each XORed with its own constant. */
#define FWI_HEADER_LEN		16
#define FWI_XOR_FIRST		0x5a
#define FWI_XOR_SECOND		0xa5

static void spi_reset(struct clp_dev *dev)
{
	clp_wr0(dev, CLP_SPI_SRR, 0x0a);
	udelay(100);
	clp_wr0(dev, CLP_SPI_CR, CLP_SPI_CR_RESET);
	udelay(10);
	clp_wr0(dev, CLP_SPI_CR, CLP_SPI_CR_INHIBIT);
}

static int spi_wait_tx_empty(struct clp_dev *dev)
{
	u32 sr;

	return read_poll_timeout(clp_rd0, sr, sr & CLP_SPI_SR_TX_EMPTY,
				 0, 10000, false, dev, CLP_SPI_SR);
}

/*
 * One short full-duplex transaction: shift out @tx, keep the last @rx_len
 * bytes clocked back. Fits in the 16-byte FIFO.
 */
static int spi_xfer(struct clp_dev *dev, const u8 *tx, int tx_len,
		    u8 *rx, int rx_len)
{
	int i, ret;

	clp_wr0(dev, CLP_SPI_SSR, ~1u);
	for (i = 0; i < tx_len; i++)
		clp_wr0(dev, CLP_SPI_DTR, tx[i]);
	clp_wr0(dev, CLP_SPI_CR, CLP_SPI_CR_RUN);
	ret = spi_wait_tx_empty(dev);
	udelay(100);
	clp_wr0(dev, CLP_SPI_CR, CLP_SPI_CR_INHIBIT);
	clp_wr0(dev, CLP_SPI_SSR, ~0u);

	for (i = 0; i < tx_len; i++) {
		u8 b = clp_rd0(dev, CLP_SPI_DRR);

		if (rx && i >= tx_len - rx_len)
			rx[i - (tx_len - rx_len)] = b;
	}
	return ret;
}

static void ecp5_cmd(struct clp_dev *dev, u8 op, u8 arg)
{
	u8 cmd[4] = { op, arg, 0, 0 };

	spi_xfer(dev, cmd, sizeof(cmd), NULL, 0);
}

static u32 ecp5_read32(struct clp_dev *dev, u8 op)
{
	u8 tx[8] = { op };
	u8 rx[4] = { };

	spi_xfer(dev, tx, sizeof(tx), rx, sizeof(rx));
	return get_unaligned_be32(rx);
}

static int ecp5_wait_idle(struct clp_dev *dev, int timeout_ms)
{
	u8 tx[5] = { ECP5_LSC_CHECK_BUSY };
	u8 rx;

	while (timeout_ms-- > 0) {
		spi_xfer(dev, tx, sizeof(tx), &rx, 1);
		if (!(rx & ECP5_BUSY))
			return 0;
		msleep(1);
	}
	return -ETIMEDOUT;
}

/* BITSTREAM_BURST: command and the whole image under one chip select. */
static int ecp5_burst(struct clp_dev *dev, const u8 *data, u32 len)
{
	static const u8 cmd[4] = { ECP5_LSC_BITSTREAM_BURST };
	int ret = 0;
	u32 i;

	clp_wr0(dev, CLP_SPI_CR, CLP_SPI_CR_RESET);
	clp_wr0(dev, CLP_SPI_SSR, ~1u);
	clp_wr0(dev, CLP_SPI_CR, CLP_SPI_CR_RUN);

	for (i = 0; i < sizeof(cmd) + len && !ret; i++) {
		u8 b = i < sizeof(cmd) ? cmd[i] : data[i - sizeof(cmd)];

		clp_wr0(dev, CLP_SPI_DTR, b);
		ret = spi_wait_tx_empty(dev);
		clp_rd0(dev, CLP_SPI_DRR);
		if ((i & 0xfff) == 0)
			cond_resched();
	}

	clp_wr0(dev, CLP_SPI_CR, CLP_SPI_CR_INHIBIT);
	clp_wr0(dev, CLP_SPI_SSR, ~0u);
	return ret;
}

static int ecp5_program(struct clp_dev *dev, const u8 *bits, u32 len)
{
	struct device *d = &dev->pdev->dev;
	u32 status;
	int ret;

	/* REFRESH first: clears a half-finished earlier attempt. */
	spi_reset(dev);
	ecp5_cmd(dev, ECP5_LSC_REFRESH, 0);
	msleep(200);
	spi_reset(dev);

	ecp5_cmd(dev, ECP5_ISC_ENABLE, 0);
	msleep(1);
	ret = ecp5_wait_idle(dev, 1000);
	if (ret) {
		dev_err(d, "ECP5 ISC_ENABLE timed out (status %08x)\n",
			ecp5_read32(dev, ECP5_LSC_READ_STATUS));
		return ret;
	}

	ecp5_cmd(dev, ECP5_ISC_ERASE, 0x01);	/* erase SRAM */
	ret = ecp5_wait_idle(dev, 5000);
	if (ret) {
		dev_err(d, "ECP5 erase timed out\n");
		return ret;
	}

	ecp5_cmd(dev, ECP5_LSC_INIT_ADDRESS, 0);
	msleep(1);

	ret = ecp5_burst(dev, bits, len);
	if (ret) {
		dev_err(d, "ECP5 SPI stalled during bitstream burst\n");
		return ret;
	}

	status = ecp5_read32(dev, ECP5_LSC_READ_STATUS);
	if (status & ECP5_STATUS_ERRORS)
		dev_err(d, "ECP5 bitstream error, status %08x\n", status);

	ecp5_cmd(dev, ECP5_ISC_DISABLE, 0);
	ecp5_cmd(dev, 0xff, 0xff);		/* NOP */

	status = ecp5_read32(dev, ECP5_LSC_READ_STATUS);
	dev->fpga_status = status;
	if (!(status & ECP5_STATUS_DONE)) {
		dev_err(d, "ECP5 not configured after load, status %08x\n", status);
		return -EIO;
	}
	return 0;
}

/*
 * The payload's first stored half is the longer one when the length is
 * odd (it is: 622035 bytes). The real bitstream is the second stored half
 * followed by the first, which yields the Lattice "\xff\x00Lattice..."
 * text header and then the 0xffffbdb3 sync word.
 */
static u8 *clp_fwi_decode(const struct firmware *fw, u32 *out_len)
{
	u32 payload, first, second, i;
	const u8 *p;
	u8 *bits;

	if (fw->size <= FWI_HEADER_LEN + 2 || fw->data[0] != 0x00 ||
	    fw->data[1] != 0x11)
		return NULL;

	p = fw->data + FWI_HEADER_LEN;
	payload = fw->size - FWI_HEADER_LEN;
	first = (payload + 1) / 2;
	second = payload - first;

	bits = vmalloc(payload);
	if (!bits)
		return NULL;
	for (i = 0; i < second; i++)
		bits[i] = p[first + i] ^ FWI_XOR_SECOND;
	for (i = 0; i < first; i++)
		bits[second + i] = p[i] ^ FWI_XOR_FIRST;

	if (bits[0] != 0xff || bits[1] != 0x00) {
		vfree(bits);
		return NULL;
	}
	*out_len = payload;
	return bits;
}

int clp_fpga_init(struct clp_dev *dev)
{
	struct device *d = &dev->pdev->dev;
	const struct firmware *fw;
	u32 len;
	u8 *bits;
	int ret, attempt;

	spi_reset(dev);
	dev->fpga_idcode = ecp5_read32(dev, ECP5_READ_ID);
	dev->fpga_status = ecp5_read32(dev, ECP5_LSC_READ_STATUS);
	dev_info(d, "ECP5 IDCODE %08x, status %08x (DONE=%d)\n",
		 dev->fpga_idcode, dev->fpga_status,
		 !!(dev->fpga_status & ECP5_STATUS_DONE));

	if (dev->fpga_idcode != ECP5_IDCODE_85F)
		dev_warn(d, "unexpected ECP5 IDCODE (expected %08x)\n",
			 ECP5_IDCODE_85F);

	/* Survives a warm reboot, module reload, and even soft power-off
	 * (the slot's standby power keeps it alive). */
	if ((dev->fpga_status & ECP5_STATUS_DONE) && !fpga_reload)
		return 0;

	ret = request_firmware(&fw, CLP_FW_NAME, d);
	if (ret) {
		dev_err(d, "need /lib/firmware/%s (from Elgato's Windows driver)\n",
			CLP_FW_NAME);
		return ret;
	}
	bits = clp_fwi_decode(fw, &len);
	release_firmware(fw);
	if (!bits) {
		dev_err(d, "%s is not a valid FWI image\n", CLP_FW_NAME);
		return -EINVAL;
	}

	/* A cold card needs a moment before the config port answers. */
	msleep(1500);
	dev_info(d, "loading ECP5 bitstream (%u bytes)\n", len);
	for (attempt = 0; attempt < 3; attempt++) {
		ret = ecp5_program(dev, bits, len);
		if (!ret)
			break;
		msleep(500);
	}
	vfree(bits);

	if (!ret)
		dev_info(d, "ECP5 configured, status %08x\n", dev->fpga_status);
	return ret;
}
