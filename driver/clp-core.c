// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Elgato Cam Link Pro - PCI probe/remove and module parameters.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "clp.h"

unsigned int clp_input;
module_param_named(input, clp_input, uint, 0644);
MODULE_PARM_DESC(input, "HDMI input to capture, 0-3 (HDMI-1..4)");

unsigned int clp_pipe_ctrl = CLP_PIPE_BASE | CLP_PIPE_NATIVE;
module_param_named(pipe_ctrl, clp_pipe_ctrl, uint, 0644);
MODULE_PARM_DESC(pipe_ctrl, "BAR0 0xd0 pipeline control value (GO bit added)");

unsigned int clp_pipe_height;
module_param_named(pipe_height, clp_pipe_height, uint, 0644);
MODULE_PARM_DESC(pipe_height, "BAR0 0xc8 height (0 = from HDMI status)");

unsigned int clp_ring_desc = 512;
module_param_named(ring_desc, clp_ring_desc, uint, 0644);
MODULE_PARM_DESC(ring_desc, "DMA ring descriptors");

unsigned int clp_ring_seg = 0x1e00;
module_param_named(ring_seg, clp_ring_seg, uint, 0644);
MODULE_PARM_DESC(ring_seg, "Bytes per DMA ring descriptor");

static int clp_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *d = &pdev->dev;
	struct clp_dev *dev;
	int ret;

	/* Not devm: it must outlive unbind while the video node is open. */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	dev->pdev = pdev;
	mutex_init(&dev->iic_lock);
	mutex_init(&dev->stream_lock);
	init_waitqueue_head(&dev->cap_wq);

	ret = pcim_enable_device(pdev);
	if (ret)
		goto err_free;
	dev->bar0 = pcim_iomap_region(pdev, 0, CLP_NAME);
	dev->bar1 = pcim_iomap_region(pdev, 1, CLP_NAME);
	if (IS_ERR(dev->bar0) || IS_ERR(dev->bar1)) {
		ret = IS_ERR(dev->bar0) ? PTR_ERR(dev->bar0) : PTR_ERR(dev->bar1);
		goto err_free;
	}

	pci_set_master(pdev);
	ret = dma_set_mask_and_coherent(d, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_mask_and_coherent(d, DMA_BIT_MASK(32));
	if (ret)
		goto err_free;

	dev_info(d, "bar0 id %08x date %08x, xdma c2h0 id %08x\n",
		 clp_rd0(dev, CLP_R_ID), clp_rd0(dev, CLP_R_DATE),
		 clp_rd1(dev, XDMA_C2H(0) + XDMA_CH_ID));
	if (clp_rd1(dev, XDMA_C2H(0) + XDMA_CH_ID) >> 20 != 0x1fc)
		dev_warn(d, "BAR1 does not look like an XDMA core\n");

	/* Without the ECP5 bitstream there is no video front end at all. */
	ret = clp_fpga_init(dev);
	if (ret)
		goto err_free;

	clp_wr0(dev, CLP_R_C4, 0x000f0000);
	clp_iic_init(dev);
	clp_xdma_init(dev);

	/* From here on the v4l2_device release callback owns dev. */
	ret = clp_video_register(dev);
	if (ret)
		goto err_free;

	pci_set_drvdata(pdev, dev);
	clp_debugfs_init(dev);
	dev_info(d, "Cam Link Pro ready\n");
	return 0;

err_free:
	kfree(dev);
	return ret;
}

static void clp_remove(struct pci_dev *pdev)
{
	struct clp_dev *dev = pci_get_drvdata(pdev);

	clp_debugfs_exit(dev);
	clp_video_unregister(dev);	/* stops a running stream */
	mutex_lock(&dev->stream_lock);
	clp_stream_stop(dev);
	clp_ring_free(dev);
	vfree(dev->cap.data);
	vfree(dev->cap.seglog);
	dev->cap.data = NULL;
	dev->cap.seglog = NULL;
	mutex_unlock(&dev->stream_lock);
	v4l2_device_put(&dev->v4l2);
}

static const struct pci_device_id clp_ids[] = {
	{ PCI_DEVICE_SUB(0x12ab, 0x0710, 0x1cfa, 0x0011) },	/* Cam Link Pro */
	{ }
};
MODULE_DEVICE_TABLE(pci, clp_ids);

static struct pci_driver clp_driver = {
	.name = CLP_NAME,
	.id_table = clp_ids,
	.probe = clp_probe,
	.remove = clp_remove,
};
module_pci_driver(clp_driver);

MODULE_DESCRIPTION("Elgato Cam Link Pro PCIe HDMI capture");
MODULE_LICENSE("GPL");
