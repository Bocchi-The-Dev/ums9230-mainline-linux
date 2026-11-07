// SPDX-License-Identifier: GPL-2.0
/*
 * Unisoc UMS9230 video codec (VSP): stateless V4L2 H.264 decoder, EXPERIMENTAL.
 *
 * The register programming follows the vendor's userspace AVC library as far
 * as it has been reverse engineered (VIDEO_CODEC.md). It has never run on the
 * hardware: expect wrong pictures, timeouts or IOMMU faults until it has been
 * debugged on the phone. Only what has been understood is implemented:
 *
 *  - slice-based decoding, Annex B start codes, 8-bit 4:2:0 progressive;
 *  - intra slices (I/SI) only: the reference-picture registers are not known
 *    well enough yet, so P and B slices fail the request;
 *  - no scaling matrices (the upload path is not known; PPS flags that ask for
 *    them are rejected, an SPS-level matrix arrives as a control and is ignored).
 *
 * Output format is NV12 in one plane, followed by per-macroblock side data the
 * codec writes (its size is a guess).
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "vsp.h"
#include "vsp-regs.h"

#define VSP_DEC_TIMEOUT_MS	1000
#define VSP_MIN_W		64
#define VSP_MIN_H		64
#define VSP_MAX_W		4096
#define VSP_MAX_H		4096
#define VSP_SRC_SIZE_MAX	(2 * 1024 * 1024)
/* bytes of side data per macroblock appended to every picture (guess) */
#define VSP_AUX0_PER_MB		128
#define VSP_AUX1_PER_MB		32

struct sprd_vsp_ctx {
	struct v4l2_fh fh;
	struct sprd_vsp *vsp;
	struct v4l2_ctrl_handler hdl;
	struct v4l2_pix_format_mplane src_fmt;
	struct v4l2_pix_format_mplane dst_fmt;
	struct vb2_v4l2_buffer *src, *dst;
};

static inline struct sprd_vsp_ctx *file_to_ctx(struct file *file)
{
	return container_of(file->private_data, struct sprd_vsp_ctx, fh);
}

static inline u32 vsp_r(struct sprd_vsp *vsp, u32 reg)
{
	return readl(vsp->base + reg);
}

static inline void vsp_w(struct sprd_vsp *vsp, u32 reg, u32 val)
{
	writel(val, vsp->base + reg);
}

/* ---- picture geometry ------------------------------------------------- */

static void sprd_vsp_geometry(const struct v4l2_pix_format_mplane *f,
			      u32 *stride, u32 *aligned_h, u32 *y_size,
			      u32 *aux0_off, u32 *aux1_off, u32 *total)
{
	u32 w = ALIGN(f->width, 16), h = ALIGN(f->height, 16);
	u32 mbs = (w / 16) * (h / 16);
	u32 ys = w * h;

	*stride = w;
	*aligned_h = h;
	*y_size = ys;
	*aux0_off = ALIGN(ys + ys / 2, 256);
	*aux1_off = ALIGN(*aux0_off + mbs * VSP_AUX0_PER_MB, 256);
	*total = ALIGN(*aux1_off + mbs * VSP_AUX1_PER_MB, 4096);
}

static void sprd_vsp_fill_dst(struct v4l2_pix_format_mplane *f)
{
	u32 stride, h, ys, a0, a1, total;

	f->width = clamp(ALIGN(f->width, 16), VSP_MIN_W, VSP_MAX_W);
	f->height = clamp(ALIGN(f->height, 16), VSP_MIN_H, VSP_MAX_H);
	f->pixelformat = V4L2_PIX_FMT_NV12;
	f->field = V4L2_FIELD_NONE;
	f->num_planes = 1;
	sprd_vsp_geometry(f, &stride, &h, &ys, &a0, &a1, &total);
	f->plane_fmt[0].bytesperline = stride;
	f->plane_fmt[0].sizeimage = total;
}

static void sprd_vsp_fill_src(struct v4l2_pix_format_mplane *f)
{
	f->width = clamp(f->width, VSP_MIN_W, VSP_MAX_W);
	f->height = clamp(f->height, VSP_MIN_H, VSP_MAX_H);
	f->pixelformat = V4L2_PIX_FMT_H264_SLICE;
	f->field = V4L2_FIELD_NONE;
	f->num_planes = 1;
	f->plane_fmt[0].bytesperline = 0;
	if (!f->plane_fmt[0].sizeimage || f->plane_fmt[0].sizeimage > VSP_SRC_SIZE_MAX)
		f->plane_fmt[0].sizeimage = clamp_t(u32, f->width * f->height, SZ_64K, VSP_SRC_SIZE_MAX);
}

/* ---- ioctls ------------------------------------------------------------- */

static int vidioc_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, "sprd-vsp", sizeof(cap->driver));
	strscpy(cap->card, "Unisoc VSP H.264 decoder", sizeof(cap->card));
	return 0;
}

static int vidioc_enum_fmt_out(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_H264_SLICE;
	return 0;
}

static int vidioc_enum_fmt_cap(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_NV12;
	return 0;
}

static int vidioc_enum_framesizes(struct file *file, void *priv, struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index)
		return -EINVAL;
	if (fsize->pixel_format != V4L2_PIX_FMT_H264_SLICE &&
	    fsize->pixel_format != V4L2_PIX_FMT_NV12)
		return -EINVAL;
	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = VSP_MIN_W;
	fsize->stepwise.max_width = VSP_MAX_W;
	fsize->stepwise.step_width = 16;
	fsize->stepwise.min_height = VSP_MIN_H;
	fsize->stepwise.max_height = VSP_MAX_H;
	fsize->stepwise.step_height = 16;
	return 0;
}

static int vidioc_g_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	f->fmt.pix_mp = file_to_ctx(file)->src_fmt;
	return 0;
}

static int vidioc_g_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	f->fmt.pix_mp = file_to_ctx(file)->dst_fmt;
	return 0;
}

static int vidioc_try_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	if (f->fmt.pix_mp.pixelformat != V4L2_PIX_FMT_H264_SLICE)
		f->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264_SLICE;
	sprd_vsp_fill_src(&f->fmt.pix_mp);
	return 0;
}

static int vidioc_try_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct sprd_vsp_ctx *ctx = file_to_ctx(file);

	/* the decoded size is that of the coded stream */
	f->fmt.pix_mp.width = ctx->src_fmt.width;
	f->fmt.pix_mp.height = ctx->src_fmt.height;
	sprd_vsp_fill_dst(&f->fmt.pix_mp);
	return 0;
}

static int vidioc_s_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct sprd_vsp_ctx *ctx = file_to_ctx(file);
	struct vb2_queue *vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	int ret;

	if (vb2_is_busy(vq))
		return -EBUSY;
	ret = vidioc_try_fmt_out(file, priv, f);
	if (ret)
		return ret;
	ctx->src_fmt = f->fmt.pix_mp;
	/* propagate the size to the capture side */
	ctx->dst_fmt.width = ctx->src_fmt.width;
	ctx->dst_fmt.height = ctx->src_fmt.height;
	sprd_vsp_fill_dst(&ctx->dst_fmt);
	return 0;
}

static int vidioc_s_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct sprd_vsp_ctx *ctx = file_to_ctx(file);
	struct vb2_queue *vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	int ret;

	if (vb2_is_busy(vq))
		return -EBUSY;
	ret = vidioc_try_fmt_cap(file, priv, f);
	if (ret)
		return ret;
	ctx->dst_fmt = f->fmt.pix_mp;
	return 0;
}

static const struct v4l2_ioctl_ops sprd_vsp_ioctl_ops = {
	.vidioc_querycap = vidioc_querycap,
	.vidioc_enum_fmt_vid_out = vidioc_enum_fmt_out,
	.vidioc_enum_fmt_vid_cap = vidioc_enum_fmt_cap,
	.vidioc_enum_framesizes = vidioc_enum_framesizes,
	.vidioc_g_fmt_vid_out_mplane = vidioc_g_fmt_out,
	.vidioc_g_fmt_vid_cap_mplane = vidioc_g_fmt_cap,
	.vidioc_try_fmt_vid_out_mplane = vidioc_try_fmt_out,
	.vidioc_try_fmt_vid_cap_mplane = vidioc_try_fmt_cap,
	.vidioc_s_fmt_vid_out_mplane = vidioc_s_fmt_out,
	.vidioc_s_fmt_vid_cap_mplane = vidioc_s_fmt_cap,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,

	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/* ---- controls ------------------------------------------------------------ */

static int sprd_vsp_try_ctrl(struct v4l2_ctrl *ctrl)
{
	if (ctrl->id == V4L2_CID_STATELESS_H264_SPS) {
		const struct v4l2_ctrl_h264_sps *sps = ctrl->p_new.p_h264_sps;

		if (sps->chroma_format_idc != 1)
			return -EINVAL;
		if (sps->bit_depth_luma_minus8 || sps->bit_depth_chroma_minus8)
			return -EINVAL;
		if (!(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY) ||
		    (sps->flags & V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD))
			return -EINVAL;
	} else if (ctrl->id == V4L2_CID_STATELESS_H264_PPS) {
		const struct v4l2_ctrl_h264_pps *pps = ctrl->p_new.p_h264_pps;

		if (pps->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT)
			return -EINVAL;
		if (pps->num_slice_groups_minus1)
			return -EINVAL;
	}
	return 0;
}

static const struct v4l2_ctrl_ops sprd_vsp_ctrl_ops = {
	.try_ctrl = sprd_vsp_try_ctrl,
};

static const struct v4l2_ctrl_config sprd_vsp_ctrls[] = {
	{
		.id = V4L2_CID_STATELESS_H264_DECODE_MODE,
		.min = V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED,
		.max = V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED,
		.def = V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED,
	}, {
		.id = V4L2_CID_STATELESS_H264_START_CODE,
		.min = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
		.max = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
		.def = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
	}, {
		.id = V4L2_CID_STATELESS_H264_SPS,
		.ops = &sprd_vsp_ctrl_ops,
	}, {
		.id = V4L2_CID_STATELESS_H264_PPS,
		.ops = &sprd_vsp_ctrl_ops,
	}, {
		.id = V4L2_CID_STATELESS_H264_SLICE_PARAMS,
	}, {
		.id = V4L2_CID_STATELESS_H264_DECODE_PARAMS,
	},
};

static const void *sprd_vsp_ctrl_data(struct sprd_vsp_ctx *ctx, u32 id)
{
	struct v4l2_ctrl *c = v4l2_ctrl_find(&ctx->hdl, id);

	return c ? c->p_cur.p : NULL;
}

/* ---- queues -------------------------------------------------------------- */

static int sprd_vsp_queue_setup(struct vb2_queue *vq, unsigned int *nbufs,
				unsigned int *nplanes, unsigned int sizes[],
				struct device *alloc_devs[])
{
	struct sprd_vsp_ctx *ctx = vb2_get_drv_priv(vq);
	const struct v4l2_pix_format_mplane *f =
		V4L2_TYPE_IS_OUTPUT(vq->type) ? &ctx->src_fmt : &ctx->dst_fmt;

	if (*nplanes)
		return *nplanes == 1 && sizes[0] >= f->plane_fmt[0].sizeimage ? 0 : -EINVAL;

	*nplanes = 1;
	sizes[0] = f->plane_fmt[0].sizeimage;
	return 0;
}

static int sprd_vsp_buf_out_validate(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	vbuf->field = V4L2_FIELD_NONE;
	return 0;
}

static int sprd_vsp_buf_prepare(struct vb2_buffer *vb)
{
	struct sprd_vsp_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	const struct v4l2_pix_format_mplane *f =
		V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type) ? &ctx->src_fmt : &ctx->dst_fmt;

	if (!V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type) &&
	    vb2_plane_size(vb, 0) < f->plane_fmt[0].sizeimage)
		return -EINVAL;
	return 0;
}

static void sprd_vsp_buf_queue(struct vb2_buffer *vb)
{
	struct sprd_vsp_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static void sprd_vsp_buf_request_complete(struct vb2_buffer *vb)
{
	struct sprd_vsp_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_ctrl_request_complete(vb->req_obj.req, &ctx->hdl);
}

static int sprd_vsp_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	return 0;
}

static void sprd_vsp_stop_streaming(struct vb2_queue *vq)
{
	struct sprd_vsp_ctx *ctx = vb2_get_drv_priv(vq);
	struct vb2_v4l2_buffer *vbuf;

	for (;;) {
		if (V4L2_TYPE_IS_OUTPUT(vq->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vbuf)
			break;
		v4l2_ctrl_request_complete(vbuf->vb2_buf.req_obj.req, &ctx->hdl);
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
}

static const struct vb2_ops sprd_vsp_qops = {
	.queue_setup = sprd_vsp_queue_setup,
	.buf_prepare = sprd_vsp_buf_prepare,
	.buf_queue = sprd_vsp_buf_queue,
	.buf_out_validate = sprd_vsp_buf_out_validate,
	.buf_request_complete = sprd_vsp_buf_request_complete,
	.start_streaming = sprd_vsp_start_streaming,
	.stop_streaming = sprd_vsp_stop_streaming,
};

static int sprd_vsp_queue_init(void *priv, struct vb2_queue *src_vq, struct vb2_queue *dst_vq)
{
	struct sprd_vsp_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->ops = &sprd_vsp_qops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->vsp->lock;
	src_vq->dev = ctx->vsp->dev;
	src_vq->supports_requests = true;
	src_vq->requires_requests = true;
	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->ops = &sprd_vsp_qops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->dma_attrs = DMA_ATTR_NO_KERNEL_MAPPING;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->vsp->lock;
	dst_vq->dev = ctx->vsp->dev;
	return vb2_queue_init(dst_vq);
}

/* ---- the job -------------------------------------------------------------- */

static int sprd_vsp_bsm_wait_state(struct sprd_vsp *vsp)
{
	u32 v;

	return readl_poll_timeout_atomic(vsp->base + VSP_BSM_STATE, v,
					 v & VSP_BSM_STATE_READY, 1, 1000);
}

/* H264Dec_cfg_bsm + read_bits, as the vendor library does them */
static int sprd_vsp_bsm_setup(struct sprd_vsp *vsp, dma_addr_t src, u32 off, u32 len,
			      u32 skip_bits)
{
	u32 v;
	int ret;

	ret = readl_poll_timeout_atomic(vsp->base + VSP_BSM_BUSY, v,
					!(v & VSP_BSM_BUSY_BIT), 1, 1000);
	if (ret)
		return ret;

	vsp_w(vsp, VSP_BSM_ADDR0, src >> 4);
	vsp_w(vsp, VSP_BSM_ADDR1, src >> 4);
	vsp_w(vsp, VSP_BSM_CMD, 6);
	vsp_w(vsp, VSP_BSM_LEN, len | VSP_BSM_START);
	vsp_w(vsp, VSP_BSM_CTRL, (off + 0x400) | VSP_BSM_START);

	while (skip_bits) {
		u32 n = min(skip_bits, 32U);

		ret = sprd_vsp_bsm_wait_state(vsp);
		if (ret)
			return ret;
		vsp_w(vsp, VSP_BSM_CMD, n << 24);
		vsp_r(vsp, VSP_BSM_DATA);
		vsp_w(vsp, VSP_BSM_CMD, (n << 24) | 1);
		skip_bits -= n;
	}

	return 0;
}

static u32 sprd_vsp_s5(int v)
{
	return v & 0x1f;
}

static int sprd_vsp_program(struct sprd_vsp_ctx *ctx)
{
	struct sprd_vsp *vsp = ctx->vsp;
	const struct v4l2_ctrl_h264_sps *sps = sprd_vsp_ctrl_data(ctx, V4L2_CID_STATELESS_H264_SPS);
	const struct v4l2_ctrl_h264_pps *pps = sprd_vsp_ctrl_data(ctx, V4L2_CID_STATELESS_H264_PPS);
	const struct v4l2_ctrl_h264_slice_params *sl =
		sprd_vsp_ctrl_data(ctx, V4L2_CID_STATELESS_H264_SLICE_PARAMS);
	u32 w_mbs, h_mbs, qp, v, stride, ah, ys, a0, a1, total, nal_off, i;
	dma_addr_t src, dst;
	const u8 *bs;
	u32 payload;
	int ret;

	if (!sps || !pps || !sl)
		return -EINVAL;

	/* only intra slices: the reference registers are not understood yet */
	if (sl->slice_type != V4L2_H264_SLICE_TYPE_I &&
	    sl->slice_type != V4L2_H264_SLICE_TYPE_SI) {
		dev_warn_once(vsp->dev, "only I slices are supported so far\n");
		return -EOPNOTSUPP;
	}

	w_mbs = sps->pic_width_in_mbs_minus1 + 1;
	h_mbs = sps->pic_height_in_map_units_minus1 + 1;
	if (w_mbs * 16 > ctx->dst_fmt.width || h_mbs * 16 > ctx->dst_fmt.height)
		return -EINVAL;

	src = vb2_dma_contig_plane_dma_addr(&ctx->src->vb2_buf, 0);
	dst = vb2_dma_contig_plane_dma_addr(&ctx->dst->vb2_buf, 0);
	payload = vb2_get_plane_payload(&ctx->src->vb2_buf, 0);
	bs = vb2_plane_vaddr(&ctx->src->vb2_buf, 0);
	if (!bs || payload < 5)
		return -EINVAL;

	/* skip the Annex B start code; the NAL header byte is part of header_bit_size */
	nal_off = (bs[0] == 0 && bs[1] == 0 && bs[2] == 0 && bs[3] == 1) ? 4 :
		  (bs[0] == 0 && bs[1] == 0 && bs[2] == 1) ? 3 : 0;
	if (!nal_off)
		return -EINVAL;

	sprd_vsp_geometry(&ctx->dst_fmt, &stride, &ah, &ys, &a0, &a1, &total);

	vsp_w(vsp, VSP_INT_CTRL, 0);

	ret = sprd_vsp_bsm_setup(vsp, src, nal_off, payload - nal_off, sl->header_bit_size);
	if (ret)
		return ret;

	/* current picture */
	vsp_w(vsp, VSP_CUR_LUMA, dst >> 4);
	vsp_w(vsp, VSP_CUR_CHROMA, (dst + ys) >> 4);
	vsp_w(vsp, VSP_CUR_AUX0, (dst + a0) >> 4);
	vsp_w(vsp, VSP_CUR_AUX1, (dst + a1) >> 4);

	qp = 26 + pps->pic_init_qp_minus26 + sl->slice_qp_delta;

	vsp_w(vsp, VSP_PARAM_UNKNOWN0, 0);
	vsp_w(vsp, VSP_PARAM_SLICE_QP, VSP_QP_VALID | (qp & 0x7f) << VSP_QP_SHIFT);

	v = (sps->profile_idc != 66 ? VSP_SEQ_NOT_BASELINE : 0) |
	    FIELD_PREP(VSP_SEQ_FIRST_MB, sl->first_mb_in_slice) |
	    FIELD_PREP(VSP_SEQ_MB_ROW, sl->first_mb_in_slice / w_mbs) |
	    FIELD_PREP(VSP_SEQ_MB_COL, sl->first_mb_in_slice % w_mbs);
	vsp_w(vsp, VSP_PARAM_SEQ, v);

	v = FIELD_PREP(VSP_FL_DEBLOCK_IDC, sl->disable_deblocking_filter_idc) |
	    FIELD_PREP(VSP_FL_ALPHA, sprd_vsp_s5(sl->slice_alpha_c0_offset_div2)) |
	    FIELD_PREP(VSP_FL_BETA, sprd_vsp_s5(sl->slice_beta_offset_div2));
	if (pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE)
		v |= VSP_FL_CABAC;
	if (pps->flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE)
		v |= VSP_FL_T8X8;
	if (pps->flags & V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED)
		v |= VSP_FL_CONSTR_INTRA;
	if (sps->flags & V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE)
		v |= VSP_FL_DIRECT8X8;
	if (pps->flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED)
		v |= VSP_FL_WEIGHTED_PRED;
	v |= FIELD_PREP(VSP_FL_BIPRED_IDC, pps->weighted_bipred_idc);
	vsp_w(vsp, VSP_PARAM_FLAGS, v);

	v = FIELD_PREP(VSP_CQP_CB, sprd_vsp_s5(pps->chroma_qp_index_offset)) |
	    FIELD_PREP(VSP_CQP_CR, sprd_vsp_s5(pps->second_chroma_qp_index_offset));
	vsp_w(vsp, VSP_PARAM_CHROMA_QP, v);

	vsp_w(vsp, VSP_PARAM_UNKNOWN1, 0);
	vsp_w(vsp, VSP_PARAM_UNKNOWN2, 0);
	vsp_w(vsp, VSP_PARAM_UNKNOWN3, 0);
	vsp_w(vsp, VSP_PARAM_UNKNOWN4, 0);
	vsp_w(vsp, VSP_PARAM_UNKNOWN5, 0);

	/* reference index tables: unused by intra slices */
	for (i = VSP_PARAM_REFIDX_FIRST; i <= VSP_PARAM_REFIDX_LAST; i += 4)
		vsp_w(vsp, i, 0);
	for (i = 0x1160; i <= 0x1170; i += 4)
		vsp_w(vsp, i, 0);
	for (i = 0x1184; i <= 0x1198; i += 4)
		vsp_w(vsp, i, 0);

	vsp_w(vsp, VSP_PARAM_PICSIZE, FIELD_PREP(VSP_PICSIZE_H_MBS, h_mbs) | (w_mbs * 16));

	return 0;
}

/* take ownership of the running job; whoever gets it (IRQ or watchdog) finishes it */
static struct sprd_vsp_ctx *sprd_vsp_claim(struct sprd_vsp *vsp)
{
	struct sprd_vsp_ctx *ctx;
	unsigned long flags;

	spin_lock_irqsave(&vsp->job_lock, flags);
	ctx = vsp->cur_ctx;
	vsp->cur_ctx = NULL;
	spin_unlock_irqrestore(&vsp->job_lock, flags);
	return ctx;
}

static void sprd_vsp_finish(struct sprd_vsp *vsp, struct sprd_vsp_ctx *ctx,
			    enum vb2_buffer_state state)
{
	cancel_delayed_work(&vsp->watchdog);
	vsp_w(vsp, VSP_INT_CTRL, 0);

	if (state == VB2_BUF_STATE_DONE)
		vb2_set_plane_payload(&ctx->dst->vb2_buf, 0,
				      ctx->dst_fmt.plane_fmt[0].sizeimage);

	v4l2_m2m_buf_done_and_job_finish(vsp->m2m_dev, ctx->fh.m2m_ctx, state);
	pm_runtime_put(vsp->dev);
}

static irqreturn_t sprd_vsp_irq(int irq, void *data)
{
	struct sprd_vsp *vsp = data;
	struct sprd_vsp_ctx *ctx;
	u32 status;

	ctx = sprd_vsp_claim(vsp);
	if (!ctx)
		return IRQ_NONE;

	status = vsp_r(vsp, VSP_INT_STATUS);
	dev_dbg(vsp->dev, "decode interrupt, status %#x\n", status);
	/* which bits mean "error" is not known yet: any interrupt completes the job */
	sprd_vsp_finish(vsp, ctx, VB2_BUF_STATE_DONE);

	return IRQ_HANDLED;
}

static void sprd_vsp_watchdog(struct work_struct *work)
{
	struct sprd_vsp *vsp = container_of(work, struct sprd_vsp, watchdog.work);
	struct sprd_vsp_ctx *ctx = sprd_vsp_claim(vsp);

	/* the interrupt got there first: nothing to do (and do not reset a later job) */
	if (!ctx)
		return;

	dev_err(vsp->dev, "decode timeout (status %#x, bsm busy %#x)\n",
		vsp_r(vsp, VSP_INT_STATUS), vsp_r(vsp, VSP_BSM_BUSY));
	vsp_w(vsp, VSP_RESET, 0xd0);
	sprd_vsp_finish(vsp, ctx, VB2_BUF_STATE_ERROR);
}

static void sprd_vsp_device_run(void *priv)
{
	struct sprd_vsp_ctx *ctx = priv;
	struct sprd_vsp *vsp = ctx->vsp;
	struct media_request *req;
	unsigned long flags;
	int ret;

	ctx->src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	ctx->dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	req = ctx->src->vb2_buf.req_obj.req;
	if (req)
		v4l2_ctrl_request_setup(req, &ctx->hdl);

	ret = pm_runtime_resume_and_get(vsp->dev);
	if (ret < 0)
		goto err;

	v4l2_m2m_buf_copy_metadata(ctx->src, ctx->dst);

	ret = sprd_vsp_program(ctx);
	if (ret) {
		pm_runtime_put(vsp->dev);
		goto err;
	}

	spin_lock_irqsave(&vsp->job_lock, flags);
	vsp->cur_ctx = ctx;
	spin_unlock_irqrestore(&vsp->job_lock, flags);

	if (req)
		v4l2_ctrl_request_complete(req, &ctx->hdl);

	schedule_delayed_work(&vsp->watchdog, msecs_to_jiffies(VSP_DEC_TIMEOUT_MS));

	/* enable the interrupt, then start (H264DecDecode_NALU) */
	vsp_w(vsp, VSP_INT_MASK, 0xffffffff);
	vsp_w(vsp, VSP_INT_CTRL, 1);
	vsp_w(vsp, VSP_START, 0xa);
	return;

err:
	if (req)
		v4l2_ctrl_request_complete(req, &ctx->hdl);
	v4l2_m2m_buf_done_and_job_finish(vsp->m2m_dev, ctx->fh.m2m_ctx, VB2_BUF_STATE_ERROR);
}

static const struct v4l2_m2m_ops sprd_vsp_m2m_ops = {
	.device_run = sprd_vsp_device_run,
};

/* ---- file operations ------------------------------------------------------- */

static int sprd_vsp_open(struct file *file)
{
	struct sprd_vsp *vsp = video_drvdata(file);
	struct sprd_vsp_ctx *ctx;
	unsigned int i;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->vsp = vsp;
	v4l2_fh_init(&ctx->fh, video_devdata(file));
	ctx->fh.ctrl_handler = &ctx->hdl;

	ctx->src_fmt.width = 640;
	ctx->src_fmt.height = 480;
	sprd_vsp_fill_src(&ctx->src_fmt);
	ctx->dst_fmt.width = ctx->src_fmt.width;
	ctx->dst_fmt.height = ctx->src_fmt.height;
	sprd_vsp_fill_dst(&ctx->dst_fmt);

	v4l2_ctrl_handler_init(&ctx->hdl, ARRAY_SIZE(sprd_vsp_ctrls));
	for (i = 0; i < ARRAY_SIZE(sprd_vsp_ctrls); i++) {
		v4l2_ctrl_new_custom(&ctx->hdl, &sprd_vsp_ctrls[i], NULL);
		if (ctx->hdl.error) {
			ret = ctx->hdl.error;
			goto err_hdl;
		}
	}
	ret = v4l2_ctrl_handler_setup(&ctx->hdl);
	if (ret)
		goto err_hdl;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(vsp->m2m_dev, ctx, sprd_vsp_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_hdl;
	}

	file->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh, file);
	return 0;

err_hdl:
	v4l2_ctrl_handler_free(&ctx->hdl);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return ret;
}

static int sprd_vsp_release(struct file *file)
{
	struct sprd_vsp_ctx *ctx = file_to_ctx(file);

	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	v4l2_ctrl_handler_free(&ctx->hdl);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations sprd_vsp_fops = {
	.owner = THIS_MODULE,
	.open = sprd_vsp_open,
	.release = sprd_vsp_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static const struct media_device_ops sprd_vsp_media_ops = {
	.req_validate = vb2_request_validate,
	.req_queue = v4l2_m2m_request_queue,
};

int sprd_vsp_dec_register(struct sprd_vsp *vsp)
{
	struct video_device *vdev = &vsp->vdev;
	int ret;

	mutex_init(&vsp->lock);
	spin_lock_init(&vsp->job_lock);
	INIT_DELAYED_WORK(&vsp->watchdog, sprd_vsp_watchdog);

	vsp->irq = platform_get_irq(to_platform_device(vsp->dev), 0);
	if (vsp->irq < 0)
		return vsp->irq;
	ret = devm_request_irq(vsp->dev, vsp->irq, sprd_vsp_irq, 0, "sprd-vsp", vsp);
	if (ret)
		return ret;

	ret = v4l2_device_register(vsp->dev, &vsp->v4l2_dev);
	if (ret)
		return ret;

	vsp->m2m_dev = v4l2_m2m_init(&sprd_vsp_m2m_ops);
	if (IS_ERR(vsp->m2m_dev)) {
		ret = PTR_ERR(vsp->m2m_dev);
		goto err_v4l2;
	}

	vsp->mdev.dev = vsp->dev;
	strscpy(vsp->mdev.model, "sprd-vsp", sizeof(vsp->mdev.model));
	media_device_init(&vsp->mdev);
	vsp->mdev.ops = &sprd_vsp_media_ops;
	vsp->v4l2_dev.mdev = &vsp->mdev;

	vdev->fops = &sprd_vsp_fops;
	vdev->ioctl_ops = &sprd_vsp_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->lock = &vsp->lock;
	vdev->v4l2_dev = &vsp->v4l2_dev;
	vdev->vfl_dir = VFL_DIR_M2M;
	vdev->device_caps = V4L2_CAP_STREAMING | V4L2_CAP_VIDEO_M2M_MPLANE;
	strscpy(vdev->name, "sprd-vsp-dec", sizeof(vdev->name));
	video_set_drvdata(vdev, vsp);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_m2m;

	ret = v4l2_m2m_register_media_controller(vsp->m2m_dev, vdev,
						 MEDIA_ENT_F_PROC_VIDEO_DECODER);
	if (ret)
		goto err_vdev;

	ret = media_device_register(&vsp->mdev);
	if (ret)
		goto err_mc;

	vsp->v4l2_registered = true;
	dev_info(vsp->dev, "experimental H.264 decoder registered as /dev/video%d\n", vdev->num);
	return 0;

err_mc:
	v4l2_m2m_unregister_media_controller(vsp->m2m_dev);
err_vdev:
	video_unregister_device(vdev);
err_m2m:
	media_device_cleanup(&vsp->mdev);
	v4l2_m2m_release(vsp->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&vsp->v4l2_dev);
	return ret;
}

void sprd_vsp_dec_unregister(struct sprd_vsp *vsp)
{
	if (!vsp->v4l2_registered)
		return;

	cancel_delayed_work_sync(&vsp->watchdog);
	media_device_unregister(&vsp->mdev);
	v4l2_m2m_unregister_media_controller(vsp->m2m_dev);
	video_unregister_device(&vsp->vdev);
	media_device_cleanup(&vsp->mdev);
	v4l2_m2m_release(vsp->m2m_dev);
	v4l2_device_unregister(&vsp->v4l2_dev);
}
