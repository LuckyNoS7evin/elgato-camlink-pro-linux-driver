// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * V4L2 capture node: YUYV at the source's resolution, filled by the framer
 * from the DMA thread.
 */
#include <linux/slab.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-vmalloc.h>

#include "clp.h"

#define CLP_MAX_WIDTH	1920
#define CLP_MAX_HEIGHT	1080

static struct clp_buffer *to_clp_buffer(struct vb2_buffer *vb)
{
	return container_of(to_vb2_v4l2_buffer(vb), struct clp_buffer, vb);
}

/* Refresh width/height from the HDMI status, unless buffers exist. */
static void clp_video_detect(struct clp_dev *dev)
{
	struct clp_hdmi_status st;

	if (vb2_is_busy(&dev->queue))
		return;
	if (clp_mcu_hdmi_status(dev, clp_input % CLP_INPUTS, &st) || !st.locked)
		return;
	if (st.width > CLP_MAX_WIDTH || st.height > CLP_MAX_HEIGHT || st.width & 1) {
		dev_warn_once(&dev->pdev->dev,
			      "%ux%u input not supported yet (max %ux%u)\n",
			      st.width, st.height, CLP_MAX_WIDTH, CLP_MAX_HEIGHT);
		return;
	}
	dev->width = st.width;
	dev->height = st.height;
	dev->htotal = st.htotal;
	dev->vtotal = st.vtotal;
}

/*
 * Nominal rate from the CEA-861 totals; 2640x1125 is both p50 and p25 (and
 * 2200x1125 both p60 and p30), so a measured rate wins when it disagrees.
 */
static void clp_frame_interval(struct clp_dev *dev, struct v4l2_fract *tpf)
{
	u32 fps = 30;
	u32 measured_us = READ_ONCE(dev->frame_interval_us);

	if (dev->vtotal == 1125 || dev->vtotal == 750)
		fps = dev->htotal == 2640 || dev->htotal == 1980 ? 50 :
		      dev->htotal == 2750 ? 24 : 60;
	if (measured_us) {
		u32 m = DIV_ROUND_CLOSEST(1000000, measured_us);

		if (m && (m * 10 < fps * 9 || m * 10 > fps * 11))
			fps = m;
	}
	tpf->numerator = 1;
	tpf->denominator = fps;
}

/* ---- framer callbacks (DMA thread) ---- */

static void *clp_get_buffer(void *ctx, u8 **dst)
{
	struct clp_dev *dev = ctx;
	struct clp_buffer *buf;
	unsigned long flags;

	spin_lock_irqsave(&dev->qlock, flags);
	buf = list_first_entry_or_null(&dev->queued, struct clp_buffer, list);
	if (buf)
		list_del(&buf->list);
	spin_unlock_irqrestore(&dev->qlock, flags);

	if (buf)
		*dst = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
	return buf;
}

static void clp_put_buffer(void *ctx, void *b, bool complete)
{
	struct clp_dev *dev = ctx;
	struct clp_buffer *buf = b;
	unsigned long flags;
	u64 now;

	if (!complete) {
		if (!dev->video_active) {	/* stream is stopping */
			vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
			return;
		}
		/* Torn frame: reuse the buffer for the next one. */
		spin_lock_irqsave(&dev->qlock, flags);
		list_add(&buf->list, &dev->queued);
		spin_unlock_irqrestore(&dev->qlock, flags);
		return;
	}

	now = ktime_get_ns();
	if (dev->last_frame_ns)
		WRITE_ONCE(dev->frame_interval_us,
			   div_u64(now - dev->last_frame_ns, 1000));
	dev->last_frame_ns = now;

	buf->vb.vb2_buf.timestamp = now;
	buf->vb.sequence = dev->sequence++;
	buf->vb.field = V4L2_FIELD_NONE;
	vb2_set_plane_payload(&buf->vb.vb2_buf, 0, dev->width * dev->height * 2);
	vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
}

static const struct clp_framer_ops clp_framer_ops = {
	.get = clp_get_buffer,
	.put = clp_put_buffer,
};

/* ---- vb2 ---- */

static int clp_queue_setup(struct vb2_queue *q, unsigned int *nbuffers,
			   unsigned int *nplanes, unsigned int sizes[],
			   struct device *alloc_devs[])
{
	struct clp_dev *dev = vb2_get_drv_priv(q);
	unsigned int size = dev->width * dev->height * 2;

	if (*nplanes)
		return sizes[0] < size ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = size;
	return 0;
}

static int clp_buf_prepare(struct vb2_buffer *vb)
{
	struct clp_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	unsigned long size = dev->width * dev->height * 2;

	if (vb2_plane_size(vb, 0) < size)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, size);
	return 0;
}

static void clp_buf_queue(struct vb2_buffer *vb)
{
	struct clp_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	struct clp_buffer *buf = to_clp_buffer(vb);
	unsigned long flags;

	spin_lock_irqsave(&dev->qlock, flags);
	list_add_tail(&buf->list, &dev->queued);
	spin_unlock_irqrestore(&dev->qlock, flags);
}

static void clp_return_buffers(struct clp_dev *dev, enum vb2_buffer_state state)
{
	struct clp_buffer *buf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&dev->qlock, flags);
	list_for_each_entry_safe(buf, tmp, &dev->queued, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&dev->qlock, flags);
}

static int clp_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct clp_dev *dev = vb2_get_drv_priv(q);
	int ret;

	mutex_lock(&dev->stream_lock);
	if (dev->streaming) {
		ret = -EBUSY;		/* a debugfs capture is running */
		goto out;
	}
	dev->sequence = 0;
	dev->last_frame_ns = 0;
	dev->frame_interval_us = 0;
	clp_framer_init(&dev->framer, dev->width, dev->height,
			clp_input % CLP_INPUTS, &clp_framer_ops, dev);
	dev->video_active = true;
	ret = clp_stream_start(dev, clp_input % CLP_INPUTS, dev->height);
	if (ret)
		dev->video_active = false;
out:
	mutex_unlock(&dev->stream_lock);
	if (ret)
		clp_return_buffers(dev, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void clp_stop_streaming(struct vb2_queue *q)
{
	struct clp_dev *dev = vb2_get_drv_priv(q);

	mutex_lock(&dev->stream_lock);
	clp_stream_stop(dev);		/* DMA thread is gone after this */
	dev->video_active = false;
	clp_framer_flush(&dev->framer);
	mutex_unlock(&dev->stream_lock);
	clp_return_buffers(dev, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops clp_vb2_ops = {
	.queue_setup = clp_queue_setup,
	.buf_prepare = clp_buf_prepare,
	.buf_queue = clp_buf_queue,
	.start_streaming = clp_start_streaming,
	.stop_streaming = clp_stop_streaming,
};

/* ---- ioctls ---- */

static int clp_querycap(struct file *file, void *priv,
			struct v4l2_capability *cap)
{
	strscpy(cap->driver, CLP_NAME, sizeof(cap->driver));
	strscpy(cap->card, "Elgato Cam Link Pro", sizeof(cap->card));
	return 0;
}

static int clp_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_YUYV;
	return 0;
}

static int clp_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct clp_dev *dev = video_drvdata(file);
	struct v4l2_pix_format *pix = &f->fmt.pix;

	clp_video_detect(dev);
	pix->width = dev->width;
	pix->height = dev->height;
	pix->pixelformat = V4L2_PIX_FMT_YUYV;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = dev->width * 2;
	pix->sizeimage = dev->width * dev->height * 2;
	pix->colorspace = dev->height >= 720 ? V4L2_COLORSPACE_REC709 :
					       V4L2_COLORSPACE_SMPTE170M;
	pix->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	return 0;
}

/* The format follows the source; any request gets the current one. */
static int clp_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct clp_dev *dev = video_drvdata(file);

	if (vb2_is_busy(&dev->queue))
		return -EBUSY;
	return clp_g_fmt(file, priv, f);
}

static int clp_enum_framesizes(struct file *file, void *priv,
			       struct v4l2_frmsizeenum *fs)
{
	struct clp_dev *dev = video_drvdata(file);

	if (fs->index || fs->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;
	clp_video_detect(dev);
	fs->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fs->discrete.width = dev->width;
	fs->discrete.height = dev->height;
	return 0;
}

static int clp_enum_frameintervals(struct file *file, void *priv,
				   struct v4l2_frmivalenum *fi)
{
	struct clp_dev *dev = video_drvdata(file);

	if (fi->index || fi->pixel_format != V4L2_PIX_FMT_YUYV ||
	    fi->width != dev->width || fi->height != dev->height)
		return -EINVAL;
	fi->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	clp_frame_interval(dev, &fi->discrete);
	return 0;
}

static int clp_g_parm(struct file *file, void *priv, struct v4l2_streamparm *p)
{
	struct clp_dev *dev = video_drvdata(file);

	if (p->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	p->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	p->parm.capture.readbuffers = 3;
	clp_frame_interval(dev, &p->parm.capture.timeperframe);
	return 0;
}

static int clp_enum_input(struct file *file, void *priv, struct v4l2_input *i)
{
	struct clp_dev *dev = video_drvdata(file);
	struct clp_hdmi_status st;

	if (i->index)
		return -EINVAL;
	snprintf(i->name, sizeof(i->name), "HDMI-%u", clp_input % CLP_INPUTS + 1);
	i->type = V4L2_INPUT_TYPE_CAMERA;
	if (clp_mcu_hdmi_status(dev, clp_input % CLP_INPUTS, &st) || !st.locked)
		i->status = V4L2_IN_ST_NO_SIGNAL;
	return 0;
}

static int clp_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int clp_s_input(struct file *file, void *priv, unsigned int i)
{
	return i ? -EINVAL : 0;
}

static const struct v4l2_ioctl_ops clp_ioctl_ops = {
	.vidioc_querycap = clp_querycap,
	.vidioc_enum_fmt_vid_cap = clp_enum_fmt,
	.vidioc_g_fmt_vid_cap = clp_g_fmt,
	.vidioc_s_fmt_vid_cap = clp_s_fmt,
	.vidioc_try_fmt_vid_cap = clp_g_fmt,
	.vidioc_enum_framesizes = clp_enum_framesizes,
	.vidioc_enum_frameintervals = clp_enum_frameintervals,
	.vidioc_g_parm = clp_g_parm,
	.vidioc_s_parm = clp_g_parm,
	.vidioc_enum_input = clp_enum_input,
	.vidioc_g_input = clp_g_input,
	.vidioc_s_input = clp_s_input,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations clp_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

/* Frees the device once the last user of the video node is gone. */
static void clp_v4l2_release(struct v4l2_device *v4l2)
{
	struct clp_dev *dev = container_of(v4l2, struct clp_dev, v4l2);

	kfree(dev);
}

int clp_video_register(struct clp_dev *dev)
{
	struct video_device *vdev = &dev->vdev;
	struct vb2_queue *q = &dev->queue;
	int ret;

	dev->width = CLP_MAX_WIDTH;
	dev->height = CLP_MAX_HEIGHT;
	mutex_init(&dev->vlock);
	spin_lock_init(&dev->qlock);
	INIT_LIST_HEAD(&dev->queued);

	dev->v4l2.release = clp_v4l2_release;
	ret = v4l2_device_register(&dev->pdev->dev, &dev->v4l2);
	if (ret)
		return ret;

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF | VB2_READ;
	q->drv_priv = dev;
	q->buf_struct_size = sizeof(struct clp_buffer);
	q->ops = &clp_vb2_ops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_queued_buffers = 2;
	q->lock = &dev->vlock;
	q->dev = &dev->pdev->dev;
	ret = vb2_queue_init(q);
	if (ret)
		goto err;

	strscpy(vdev->name, "Elgato Cam Link Pro", sizeof(vdev->name));
	vdev->v4l2_dev = &dev->v4l2;
	vdev->fops = &clp_fops;
	vdev->ioctl_ops = &clp_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->lock = &dev->vlock;
	vdev->queue = q;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
			    V4L2_CAP_READWRITE;
	video_set_drvdata(vdev, dev);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err;
	dev_info(&dev->pdev->dev, "registered %s\n", video_device_node_name(vdev));
	return 0;

err:
	v4l2_device_unregister(&dev->v4l2);
	return ret;
}

/* Stops any stream; the device is freed when the last file is closed. */
void clp_video_unregister(struct clp_dev *dev)
{
	vb2_video_unregister_device(&dev->vdev);
	v4l2_device_unregister(&dev->v4l2);
}
