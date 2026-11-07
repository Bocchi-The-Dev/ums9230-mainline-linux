// SPDX-License-Identifier: GPL-2.0
/*
 * Register lab for the Unisoc JPEG codec (and any other block bound by sprd-vsp
 * that has no decoder yet), debugfs only.
 *
 * The JPEG decode sequence has been read out of the vendor library but never
 * run; this lets it be replayed one command at a time from a shell, with
 * DMA buffers behind the codec's IOMMU, so that each guess can be checked on
 * the phone before a real driver depends on it.
 *
 *   echo "on" > /sys/kernel/debug/sprd-jpg/lab_cmd      power the block, keep it on
 *   cat /sys/kernel/debug/sprd-jpg/lab_out               results of the commands so far
 *
 * Commands (numbers are decimal or 0x hex; one per write, or several lines):
 *   on | off                     hold / drop the power reference
 *   w OFF VAL                    write a register, then read it back (reported)
 *   r OFF [COUNT]                read COUNT words (default 1)
 *   poll OFF MASK EXP [MS]       wait until (reg & MASK) == EXP (default 100 ms)
 *   buf N SIZE                   (re)allocate DMA buffer N (0-3), reports its bus address
 *   wb OFF N [SHIFT [ADD]]       write (bus address of buffer N + ADD) >> SHIFT to a register
 *   bufdump N [OFF [LEN]]        hex dump of a buffer into the output
 *   irq [MS]                     wait for the next interrupt, report its status (reg 0x2c)
 *   clear                        empty the output
 * Buffers are also readable and writable as files lab_buf0..3 (cat / dd).
 */

#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "vsp.h"

#define LAB_NBUF	4
#define LAB_OUT_SIZE	(64 * 1024)
#define LAB_INT_STATUS	0x2c
#define LAB_INT_CLEAR	0x28

struct sprd_vsp_lab_buf {
	void *cpu;
	dma_addr_t dma;
	size_t size;
};

struct sprd_vsp_lab {
	struct sprd_vsp *vsp;
	struct mutex lock;
	struct sprd_vsp_lab_buf buf[LAB_NBUF];
	char *out;
	size_t out_len;
	bool powered;
	int irq;
	struct completion irq_done;
	u32 irq_status;
};

static void lab_log(struct sprd_vsp_lab *lab, const char *fmt, ...)
{
	va_list args;
	int n;

	if (lab->out_len >= LAB_OUT_SIZE - 1)
		return;
	va_start(args, fmt);
	n = vscnprintf(lab->out + lab->out_len, LAB_OUT_SIZE - lab->out_len, fmt, args);
	va_end(args);
	lab->out_len += n;
}

static irqreturn_t lab_irq(int irq, void *data)
{
	struct sprd_vsp_lab *lab = data;
	u32 st;

	if (!lab->powered)
		return IRQ_NONE;
	st = readl(lab->vsp->base + LAB_INT_STATUS);
	if (!(st & 0xb))
		return IRQ_NONE;
	/* the vendor driver acknowledges by writing the bits it saw */
	writel(st & 0xb, lab->vsp->base + LAB_INT_CLEAR);
	lab->irq_status = st;
	complete(&lab->irq_done);
	return IRQ_HANDLED;
}

static int lab_power(struct sprd_vsp_lab *lab, bool on)
{
	int ret;

	if (on == lab->powered)
		return 0;
	if (on) {
		ret = pm_runtime_resume_and_get(lab->vsp->dev);
		if (ret)
			return ret;
	} else {
		pm_runtime_put(lab->vsp->dev);
	}
	lab->powered = on;
	return 0;
}

static void lab_free_buf(struct sprd_vsp_lab *lab, int n)
{
	struct sprd_vsp_lab_buf *b = &lab->buf[n];

	if (b->cpu)
		dma_free_coherent(lab->vsp->dev, b->size, b->cpu, b->dma);
	memset(b, 0, sizeof(*b));
}

static void lab_hexdump(struct sprd_vsp_lab *lab, struct sprd_vsp_lab_buf *b, u32 off, u32 len)
{
	u32 i;

	for (i = off & ~15; i < min_t(u32, off + len, b->size); i += 16) {
		const u32 *w = b->cpu + i;

		lab_log(lab, "%06x: %08x %08x %08x %08x\n", i, w[0], w[1], w[2], w[3]);
	}
}

static int lab_run(struct sprd_vsp_lab *lab, char *line)
{
	struct sprd_vsp *vsp = lab->vsp;
	unsigned long a[4] = {};
	char cmd[16];
	int n, i, nargs = 0;
	char *p, *tok;

	line = strim(line);
	if (!*line || *line == '#')
		return 0;

	p = line;
	tok = strsep(&p, " \t");
	strscpy(cmd, tok, sizeof(cmd));
	while (p && nargs < 4) {
		tok = strsep(&p, " \t");
		if (!*tok)
			continue;
		if (kstrtoul(tok, 0, &a[nargs]))
			return -EINVAL;
		nargs++;
	}

	if (!strcmp(cmd, "on"))
		return lab_power(lab, true);
	if (!strcmp(cmd, "off"))
		return lab_power(lab, false);
	if (!strcmp(cmd, "clear")) {
		lab->out_len = 0;
		return 0;
	}

	if (!lab->powered) {
		lab_log(lab, "%s: not powered, run 'on' first\n", cmd);
		return -EIO;
	}

	if (!strcmp(cmd, "w") && nargs == 2) {
		u32 rb;

		if (a[0] & 3 || a[0] >= 0xc000)
			return -EINVAL;
		writel(a[1], vsp->base + a[0]);
		rb = readl(vsp->base + a[0]);
		lab_log(lab, "w %#05lx = %#010lx -> reads %#010x%s\n", a[0], a[1], rb,
			rb == (u32)a[1] ? "" : "  (DIFFERS)");
	} else if (!strcmp(cmd, "r") && nargs >= 1) {
		n = nargs > 1 ? min(a[1], 64UL) : 1;
		if (a[0] & 3 || a[0] + n * 4 > 0xc000)
			return -EINVAL;
		for (i = 0; i < n; i++)
			lab_log(lab, "r %#05lx = %#010x\n", a[0] + i * 4,
				readl(vsp->base + a[0] + i * 4));
	} else if (!strcmp(cmd, "poll") && nargs >= 3) {
		u32 v = 0;
		int ret;

		if (a[0] & 3 || a[0] >= 0xc000)
			return -EINVAL;
		ret = read_poll_timeout(readl, v, (v & a[1]) == a[2], 100, (nargs > 3 ? a[3] : 100) * 1000,
					false, vsp->base + a[0]);
		lab_log(lab, "poll %#05lx & %#lx == %#lx: %s (reg = %#010x)\n", a[0], a[1], a[2],
			ret ? "TIMEOUT" : "ok", v);
	} else if (!strcmp(cmd, "buf") && nargs == 2) {
		struct sprd_vsp_lab_buf *b;

		if (a[0] >= LAB_NBUF || !a[1] || a[1] > SZ_16M)
			return -EINVAL;
		lab_free_buf(lab, a[0]);
		b = &lab->buf[a[0]];
		b->size = ALIGN(a[1], PAGE_SIZE);
		b->cpu = dma_alloc_coherent(vsp->dev, b->size, &b->dma, GFP_KERNEL);
		if (!b->cpu) {
			b->size = 0;
			return -ENOMEM;
		}
		lab_log(lab, "buf %lu: %zu bytes at bus address %pad\n", a[0], b->size, &b->dma);
	} else if (!strcmp(cmd, "wb") && nargs >= 2) {
		struct sprd_vsp_lab_buf *b;
		u64 v;

		if (a[0] & 3 || a[0] >= 0xc000 || a[1] >= LAB_NBUF || !lab->buf[a[1]].cpu)
			return -EINVAL;
		b = &lab->buf[a[1]];
		v = (b->dma + (nargs > 3 ? a[3] : 0)) >> (nargs > 2 ? a[2] : 0);
		writel(lower_32_bits(v), vsp->base + a[0]);
		lab_log(lab, "wb %#05lx = %#010x (buf %lu %pad >> %lu)\n", a[0],
			readl(vsp->base + a[0]), a[1], &b->dma, nargs > 2 ? a[2] : 0);
	} else if (!strcmp(cmd, "bufdump") && nargs >= 1) {
		if (a[0] >= LAB_NBUF || !lab->buf[a[0]].cpu)
			return -EINVAL;
		lab_hexdump(lab, &lab->buf[a[0]], nargs > 1 ? a[1] : 0, nargs > 2 ? a[2] : 64);
	} else if (!strcmp(cmd, "irq")) {
		unsigned long ms = nargs ? a[0] : 1000;

		if (lab->irq < 0) {
			lab_log(lab, "irq: no interrupt\n");
			return -ENODEV;
		}
		reinit_completion(&lab->irq_done);
		mutex_unlock(&lab->lock);
		n = wait_for_completion_timeout(&lab->irq_done, msecs_to_jiffies(ms));
		mutex_lock(&lab->lock);
		if (n)
			lab_log(lab, "irq: status %#010x\n", lab->irq_status);
		else
			lab_log(lab, "irq: TIMEOUT after %lu ms (reg 0x2c = %#x)\n", ms,
				readl(vsp->base + LAB_INT_STATUS));
	} else {
		lab_log(lab, "unknown or malformed command: %s\n", line);
		return -EINVAL;
	}

	return 0;
}

static ssize_t lab_cmd_write(struct file *file, const char __user *ubuf, size_t len, loff_t *ppos)
{
	struct sprd_vsp_lab *lab = file->private_data;
	char *kbuf, *line, *p;
	int ret = 0;

	if (len > PAGE_SIZE)
		return -E2BIG;
	kbuf = memdup_user_nul(ubuf, len);
	if (IS_ERR(kbuf))
		return PTR_ERR(kbuf);

	mutex_lock(&lab->lock);
	p = kbuf;
	while ((line = strsep(&p, "\n"))) {
		ret = lab_run(lab, line);
		if (ret) {
			lab_log(lab, "  -> error %d in \"%s\"\n", ret, line);
			break;
		}
	}
	mutex_unlock(&lab->lock);
	kfree(kbuf);

	return ret ? ret : len;
}

static int lab_cmd_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return nonseekable_open(inode, file);
}

static const struct file_operations lab_cmd_fops = {
	.owner = THIS_MODULE,
	.open = lab_cmd_open,
	.write = lab_cmd_write,
};

static int lab_out_show(struct seq_file *s, void *unused)
{
	struct sprd_vsp_lab *lab = s->private;

	mutex_lock(&lab->lock);
	seq_write(s, lab->out, lab->out_len);
	mutex_unlock(&lab->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lab_out);

struct lab_buf_file {
	struct sprd_vsp_lab *lab;
	int n;
};

static ssize_t lab_buf_read(struct file *file, char __user *ubuf, size_t len, loff_t *ppos)
{
	struct lab_buf_file *bf = file->private_data;
	struct sprd_vsp_lab_buf *b = &bf->lab->buf[bf->n];
	ssize_t ret;

	mutex_lock(&bf->lab->lock);
	ret = b->cpu ? simple_read_from_buffer(ubuf, len, ppos, b->cpu, b->size) : 0;
	mutex_unlock(&bf->lab->lock);
	return ret;
}

static ssize_t lab_buf_write(struct file *file, const char __user *ubuf, size_t len, loff_t *ppos)
{
	struct lab_buf_file *bf = file->private_data;
	struct sprd_vsp_lab_buf *b = &bf->lab->buf[bf->n];
	ssize_t ret = -ENOMEM;

	mutex_lock(&bf->lab->lock);
	if (b->cpu)
		ret = simple_write_to_buffer(b->cpu, b->size, ppos, ubuf, len);
	mutex_unlock(&bf->lab->lock);
	return ret;
}

static int lab_buf_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static const struct file_operations lab_buf_fops = {
	.owner = THIS_MODULE,
	.open = lab_buf_open,
	.read = lab_buf_read,
	.write = lab_buf_write,
	.llseek = default_llseek,
};

void sprd_vsp_lab_register(struct sprd_vsp *vsp)
{
	struct sprd_vsp_lab *lab;
	struct lab_buf_file *bfs;
	char name[16];
	int i, ret;

	lab = devm_kzalloc(vsp->dev, sizeof(*lab), GFP_KERNEL);
	bfs = devm_kcalloc(vsp->dev, LAB_NBUF, sizeof(*bfs), GFP_KERNEL);
	if (!lab || !bfs)
		return;
	lab->out = devm_kzalloc(vsp->dev, LAB_OUT_SIZE, GFP_KERNEL);
	if (!lab->out)
		return;

	lab->vsp = vsp;
	mutex_init(&lab->lock);
	init_completion(&lab->irq_done);
	vsp->lab = lab;

	lab->irq = platform_get_irq_optional(to_platform_device(vsp->dev), 0);
	if (lab->irq >= 0) {
		ret = devm_request_irq(vsp->dev, lab->irq, lab_irq, IRQF_SHARED, "sprd-vsp-lab", lab);
		if (ret)
			lab->irq = -1;
	}

	debugfs_create_file("lab_cmd", 0200, vsp->dir, lab, &lab_cmd_fops);
	debugfs_create_file("lab_out", 0400, vsp->dir, lab, &lab_out_fops);
	for (i = 0; i < LAB_NBUF; i++) {
		bfs[i].lab = lab;
		bfs[i].n = i;
		snprintf(name, sizeof(name), "lab_buf%d", i);
		debugfs_create_file(name, 0600, vsp->dir, &bfs[i], &lab_buf_fops);
	}
}

void sprd_vsp_lab_unregister(struct sprd_vsp *vsp)
{
	struct sprd_vsp_lab *lab = vsp->lab;
	int i;

	if (!lab)
		return;
	/* the debugfs files go first (the caller removes the directory) */
	debugfs_remove_recursive(vsp->dir);
	vsp->dir = NULL;
	mutex_lock(&lab->lock);
	for (i = 0; i < LAB_NBUF; i++)
		lab_free_buf(lab, i);
	if (lab->powered) {
		pm_runtime_put(vsp->dev);
		lab->powered = false;
	}
	mutex_unlock(&lab->lock);
}
