/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SPRD_VSP_H
#define _SPRD_VSP_H

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/reset.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <media/media-device.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mem2mem.h>

struct device;
struct dentry;
struct dev_pm_domain_list;

struct sprd_vsp_range {
	u32 start;
	u32 len;
};

struct sprd_vsp_data {
	const char *name;
	const struct sprd_vsp_range *ranges;
	int num_ranges;
	bool decoder;
};

struct sprd_vsp_ctx;
struct sprd_vsp_lab;

struct sprd_vsp {
	struct device *dev;
	const struct sprd_vsp_data *data;
	void __iomem *base;
	struct clk_bulk_data *clks;
	int num_clks;
	struct reset_control *rsts;
	struct dev_pm_domain_list *pds;
	struct dentry *dir;
	struct sprd_vsp_lab *lab;	/* register lab (vsp-lab.c), JPEG block only */

	/* stateless H.264 decoder (vsp-dec.c) */
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct media_device mdev;
	struct v4l2_m2m_dev *m2m_dev;
	struct mutex lock;		/* serialises the V4L2 ioctls */
	spinlock_t job_lock;		/* protects cur_ctx and the job state */
	struct sprd_vsp_ctx *cur_ctx;
	struct delayed_work watchdog;
	int irq;
	bool v4l2_registered;
};

int sprd_vsp_dec_register(struct sprd_vsp *vsp);
void sprd_vsp_dec_unregister(struct sprd_vsp *vsp);

void sprd_vsp_lab_register(struct sprd_vsp *vsp);
void sprd_vsp_lab_unregister(struct sprd_vsp *vsp);

#endif
