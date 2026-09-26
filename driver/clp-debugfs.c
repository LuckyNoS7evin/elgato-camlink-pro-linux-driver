// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * /sys/kernel/debug/camlinkpro/ - the reverse-engineering workbench.
 *
 *   info      FPGA/XDMA identification, ring config, last capture
 *   regs      BAR0 0x000-0x0ff and the video XDMA channel registers
 *   status    per-input HDMI status from the MCU (decoded + raw)
 *   mcu_dump  MCU register space 0x00-0xff
 *   poke      "bar0 <off> <val>", "bar1 <off> <val>" or "mcu <sub> <val>"
 *   capture   write N: stream and capture N MiB (0 = default) of raw ring data
 *   dump      the captured bytes
 *   seglog    per segment: writeback word 0 and word 1 (u32 LE pairs)
 */
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#include "clp.h"

static unsigned int capture_mb = 32;
module_param(capture_mb, uint, 0644);
MODULE_PARM_DESC(capture_mb, "Default raw capture size in MiB");

static int info_show(struct seq_file *s, void *unused)
{
	struct clp_dev *dev = s->private;
	struct clp_capture *cap = &dev->cap;
	int i;

	seq_printf(s, "bar0 id      %08x  date %08x\n",
		   clp_rd0(dev, CLP_R_ID), clp_rd0(dev, CLP_R_DATE));
	seq_printf(s, "ecp5 idcode  %08x  status %08x\n",
		   dev->fpga_idcode, dev->fpga_status);
	for (i = 0; i < 2; i++)
		seq_printf(s, "xdma c2h%d id %08x\n", i,
			   clp_rd1(dev, XDMA_C2H(i) + XDMA_CH_ID));
	seq_printf(s, "ring         %u x %u bytes, consumed %llu, overruns %llu\n",
		   dev->ring.ndesc, dev->ring.seg, dev->ring.consumed,
		   dev->ring.overruns);
	seq_printf(s, "streaming    %d (HDMI-%u, height %u)\n", dev->streaming,
		   dev->stream_input + 1, dev->stream_height);
	seq_printf(s, "capture      %zu/%zu bytes, %u segments, result %d\n",
		   cap->len, cap->size, cap->seglog_len, cap->result);
	seq_printf(s, "video        %ux%u, active %d, frame interval %u us\n",
		   dev->width, dev->height, dev->video_active,
		   dev->frame_interval_us);
	seq_printf(s, "framer       chunks %llu frames %llu torn %llu dropped %llu resyncs %llu bad headers %llu\n",
		   dev->framer.chunks, dev->framer.frames,
		   dev->framer.torn_frames, dev->framer.dropped,
		   dev->framer.resyncs, dev->framer.bad_headers);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(info);

static void dump_words(struct seq_file *s, const char *bar,
		       u32 (*rd)(struct clp_dev *, u32), struct clp_dev *dev,
		       u32 start, u32 end)
{
	u32 off;

	for (off = start; off < end; off += 0x20) {
		int i;

		seq_printf(s, "%s %04x:", bar, off);
		for (i = 0; i < 8; i++)
			seq_printf(s, " %08x", rd(dev, off + 4 * i));
		seq_putc(s, '\n');
	}
}

static int regs_show(struct seq_file *s, void *unused)
{
	struct clp_dev *dev = s->private;

	dump_words(s, "bar0", clp_rd0, dev, 0x0000, 0x0100);
	dump_words(s, "bar1", clp_rd1, dev, XDMA_C2H(0), XDMA_C2H(0) + 0x100);
	dump_words(s, "bar1", clp_rd1, dev, XDMA_SGDMA(XDMA_C2H(0)) + 0x80,
		   XDMA_SGDMA(XDMA_C2H(0)) + 0xa0);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(regs);

static int status_show(struct seq_file *s, void *unused)
{
	struct clp_dev *dev = s->private;
	struct clp_hdmi_status st;
	int i, j, ret;

	for (i = 0; i < CLP_INPUTS; i++) {
		ret = clp_mcu_hdmi_status(dev, i, &st);
		if (ret) {
			seq_printf(s, "HDMI-%d: read error %d\n", i + 1, ret);
			continue;
		}
		if (st.locked)
			seq_printf(s, "HDMI-%d: %ux%u (total %ux%u)  raw:",
				   i + 1, st.width, st.height, st.htotal,
				   st.vtotal);
		else
			seq_printf(s, "HDMI-%d: no signal  raw:", i + 1);
		for (j = 0; j < CLP_MCU_STATUS_LEN; j++)
			seq_printf(s, " %02x", st.raw[j]);
		seq_putc(s, '\n');
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

static int mcu_dump_show(struct seq_file *s, void *unused)
{
	struct clp_dev *dev = s->private;
	u8 buf[16];
	int sub, i, ret;

	for (sub = 0; sub < 0x100; sub += 16) {
		ret = clp_mcu_read(dev, sub, buf, sizeof(buf));
		seq_printf(s, "%02x:", sub);
		if (ret) {
			seq_printf(s, " error %d\n", ret);
			continue;
		}
		for (i = 0; i < 16; i++)
			seq_printf(s, " %02x", buf[i]);
		seq_putc(s, '\n');
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mcu_dump);

static ssize_t poke_write(struct file *file, const char __user *ubuf,
			  size_t count, loff_t *ppos)
{
	struct clp_dev *dev = file->private_data;
	char kbuf[64], what[8];
	u32 off, val;
	u8 b;
	int ret;

	if (count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, ubuf, count))
		return -EFAULT;
	kbuf[count] = 0;
	if (sscanf(kbuf, "%7s %x %x", what, &off, &val) != 3)
		return -EINVAL;

	if (!strcmp(what, "bar0") && off < 0x100000 && !(off & 3)) {
		clp_wr0(dev, off, val);
	} else if (!strcmp(what, "bar1") && off < 0x10000 && !(off & 3)) {
		clp_wr1(dev, off, val);
	} else if (!strcmp(what, "mcu") && off < 0x100 && val < 0x100) {
		b = val;
		ret = clp_mcu_write(dev, off, &b, 1);
		if (ret)
			return ret;
	} else {
		return -EINVAL;
	}
	dev_info(&dev->pdev->dev, "poke %s %#x = %#x\n", what, off, val);
	return count;
}

static const struct file_operations poke_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = poke_write,
};

static ssize_t capture_write(struct file *file, const char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	struct clp_dev *dev = file->private_data;
	unsigned int mb;
	int ret;

	ret = kstrtouint_from_user(ubuf, count, 0, &mb);
	if (ret)
		return ret;
	if (!mb)
		mb = capture_mb;
	if (mb > 512)
		return -EINVAL;

	mutex_lock(&dev->stream_lock);
	ret = clp_capture_run(dev, (size_t)mb << 20);
	mutex_unlock(&dev->stream_lock);
	/* A partial capture is still worth reading; only fail on no data. */
	if (ret && !dev->cap.len)
		return ret;
	return count;
}

static const struct file_operations capture_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = capture_write,
};

static ssize_t dump_read(struct file *file, char __user *ubuf, size_t count,
			 loff_t *ppos)
{
	struct clp_dev *dev = file->private_data;
	ssize_t ret;

	mutex_lock(&dev->stream_lock);
	ret = simple_read_from_buffer(ubuf, count, ppos, dev->cap.data,
				      dev->cap.len);
	mutex_unlock(&dev->stream_lock);
	return ret;
}

static const struct file_operations dump_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = dump_read,
	.llseek = default_llseek,
};

static ssize_t seglog_read(struct file *file, char __user *ubuf, size_t count,
			   loff_t *ppos)
{
	struct clp_dev *dev = file->private_data;
	ssize_t ret;

	mutex_lock(&dev->stream_lock);
	ret = simple_read_from_buffer(ubuf, count, ppos, dev->cap.seglog,
				      dev->cap.seglog_len * 2 * sizeof(u32));
	mutex_unlock(&dev->stream_lock);
	return ret;
}

static const struct file_operations seglog_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = seglog_read,
	.llseek = default_llseek,
};

void clp_debugfs_init(struct clp_dev *dev)
{
	struct dentry *d = debugfs_create_dir(CLP_NAME, NULL);

	dev->dbg = d;
	debugfs_create_file("info", 0400, d, dev, &info_fops);
	debugfs_create_file("regs", 0400, d, dev, &regs_fops);
	debugfs_create_file("status", 0400, d, dev, &status_fops);
	debugfs_create_file("mcu_dump", 0400, d, dev, &mcu_dump_fops);
	debugfs_create_file("poke", 0200, d, dev, &poke_fops);
	debugfs_create_file("capture", 0200, d, dev, &capture_fops);
	debugfs_create_file("dump", 0400, d, dev, &dump_fops);
	debugfs_create_file("seglog", 0400, d, dev, &seglog_fops);
}

void clp_debugfs_exit(struct clp_dev *dev)
{
	debugfs_remove_recursive(dev->dbg);
	dev->dbg = NULL;
}
