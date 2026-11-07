// SPDX-License-Identifier: GPL-2.0
/*
 * Unisoc UMS9230 video codec (VSP) and JPEG codec: power, clock and reset bring-up.
 *
 * This brings the block up and lets a developer look at its registers
 * through debugfs. The experimental H.264 decoder is in vsp-dec.c; the
 * register map is not documented and has to be found out step by step.
 *
 * debugfs: /sys/kernel/debug/sprd-vsp/regs (sprd-jpg/regs for the JPEG codec) powers the block, pulses its
 * reset and dumps the register ranges listed in the match data.
 */

#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/seq_file.h>

#include "vsp.h"

static int sprd_vsp_runtime_resume(struct device *dev)
{
	struct sprd_vsp *vsp = dev_get_drvdata(dev);
	int ret;

	ret = clk_bulk_prepare_enable(vsp->num_clks, vsp->clks);
	if (ret)
		return ret;

	ret = reset_control_deassert(vsp->rsts);
	if (ret)
		clk_bulk_disable_unprepare(vsp->num_clks, vsp->clks);

	return ret;
}

static int sprd_vsp_runtime_suspend(struct device *dev)
{
	struct sprd_vsp *vsp = dev_get_drvdata(dev);

	reset_control_assert(vsp->rsts);
	clk_bulk_disable_unprepare(vsp->num_clks, vsp->clks);

	return 0;
}

static int sprd_vsp_regs_show(struct seq_file *s, void *unused)
{
	struct sprd_vsp *vsp = s->private;
	int ret, r, i;

	ret = pm_runtime_resume_and_get(vsp->dev);
	if (ret)
		return ret;

	for (r = 0; r < vsp->data->num_ranges; r++) {
		const struct sprd_vsp_range *rg = &vsp->data->ranges[r];

		for (i = rg->start; i < rg->start + rg->len; i += 16)
			seq_printf(s, "%04x: %08x %08x %08x %08x\n", i,
				   readl(vsp->base + i), readl(vsp->base + i + 4),
				   readl(vsp->base + i + 8), readl(vsp->base + i + 12));
	}

	pm_runtime_put(vsp->dev);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sprd_vsp_regs);

/*
 * Allocate a buffer the way a decoder would, through the IOMMU, and check the
 * CPU view of it: the bus address has to lie in the IOMMU's window and what is
 * written has to read back. The codec itself is not involved.
 */
static int sprd_vsp_iommu_test_show(struct seq_file *s, void *unused)
{
	struct sprd_vsp *vsp = s->private;
	struct iommu_domain *dom = iommu_get_domain_for_dev(vsp->dev);
	size_t size = 256 * 1024;
	dma_addr_t dma;
	u32 *buf;
	int ret, i, bad = 0;

	seq_printf(s, "iommu domain: %s\n", dom ? (dom->type == IOMMU_DOMAIN_DMA ?
		   "DMA (translated)" : "other") : "none (no IOMMU in use)");

	ret = pm_runtime_resume_and_get(vsp->dev);
	if (ret)
		return ret;

	buf = dma_alloc_coherent(vsp->dev, size, &dma, GFP_KERNEL);
	if (!buf) {
		seq_puts(s, "dma_alloc_coherent failed\n");
		pm_runtime_put(vsp->dev);
		return 0;
	}

	for (i = 0; i < size / 4; i++)
		buf[i] = 0xa5000000 | i;
	for (i = 0; i < size / 4; i++)
		if (buf[i] != (0xa5000000 | i))
			bad++;

	seq_printf(s, "buffer: %zu bytes, bus address %pad", size, &dma);
	if (dom)
		seq_printf(s, ", physical %pa", &(phys_addr_t){ iommu_iova_to_phys(dom, dma) });
	seq_printf(s, "\nreadback mismatches: %d\n", bad);

	dma_free_coherent(vsp->dev, size, buf, dma);
	pm_runtime_put(vsp->dev);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sprd_vsp_iommu_test);

static int sprd_vsp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct sprd_vsp_data *data = device_get_match_data(dev);
	struct resource *res;
	struct sprd_vsp *vsp;
	int ret;

	vsp = devm_kzalloc(dev, sizeof(*vsp), GFP_KERNEL);
	if (!vsp)
		return -ENOMEM;

	vsp->dev = dev;
	vsp->data = data;
	platform_set_drvdata(pdev, vsp);

	/*
	 * The first 0x80 bytes of this window belong to the codec's IOMMU, which
	 * has its own node and driver, so the region cannot be claimed
	 * exclusively here.
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	vsp->base = devm_ioremap(dev, res->start, resource_size(res));
	if (!vsp->base)
		return -ENOMEM;

	/* the clocks are enabled in the order the device tree lists them */
	ret = devm_clk_bulk_get_all(dev, &vsp->clks);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get the clocks\n");
	vsp->num_clks = ret;

	vsp->rsts = devm_reset_control_array_get_shared(dev);
	if (IS_ERR(vsp->rsts))
		return dev_err_probe(dev, PTR_ERR(vsp->rsts),
				     "failed to get the resets\n");

	/*
	 * The VSP lists two power domains (multimedia and VSP) and has to attach
	 * them itself. The JPEG codec lists only the multimedia one, which the
	 * platform bus has already attached before probe, and attaching again
	 * would fail with -EEXIST.
	 */
	if (!dev->pm_domain) {
		ret = devm_pm_domain_attach_list(dev, NULL, &vsp->pds);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "failed to attach the power domains\n");
	}

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	vsp->dir = debugfs_create_dir(data->name, NULL);
	debugfs_create_file("regs", 0400, vsp->dir, vsp, &sprd_vsp_regs_fops);
	debugfs_create_file("iommu_test", 0400, vsp->dir, vsp, &sprd_vsp_iommu_test_fops);

	if (data->decoder) {
		ret = sprd_vsp_dec_register(vsp);
		if (ret)
			dev_warn(dev, "decoder not registered: %d\n", ret);
	} else {
		sprd_vsp_lab_register(vsp);
	}

	dev_info(dev, "ready; cat /sys/kernel/debug/%s/regs powers it and dumps its registers\n",
		 data->name);

	return 0;
}

static void sprd_vsp_remove(struct platform_device *pdev)
{
	struct sprd_vsp *vsp = platform_get_drvdata(pdev);

	sprd_vsp_dec_unregister(vsp);
	sprd_vsp_lab_unregister(vsp);
	debugfs_remove_recursive(vsp->dir);
}

static const struct dev_pm_ops sprd_vsp_pm_ops = {
	SET_RUNTIME_PM_OPS(sprd_vsp_runtime_suspend, sprd_vsp_runtime_resume, NULL)
};

/*
 * The first 0x80 bytes of the VSP window are the registers of its IOMMU (the
 * stock device tree puts "sprd,iommuvaul6-vsp" at 0x32000000 with size 0x80).
 * The vendor's userspace library addresses the codec itself at 0x1000-0x1060
 * (reset, bus idle, interrupt, bitstream module setup), 0x2000-0x24ff and the
 * bitstream module at 0x8000-0x8030.
 */
static const struct sprd_vsp_range sprd_vsp_vsp_ranges[] = {
	{ 0x0000, 0x0100 }, { 0x1000, 0x0080 }, { 0x1200, 0x0020 },
	{ 0x2080, 0x0020 }, { 0x2400, 0x0010 }, { 0x2480, 0x0010 },
	{ 0x8000, 0x0030 },
};
static const struct sprd_vsp_range sprd_vsp_jpg_ranges[] = {
	{ 0x0000, 0x0400 },
};

static const struct sprd_vsp_data sprd_vsp_vsp_data = {
	.name = "sprd-vsp",
	.ranges = sprd_vsp_vsp_ranges,
	.num_ranges = ARRAY_SIZE(sprd_vsp_vsp_ranges),
	.decoder = true,
};
static const struct sprd_vsp_data sprd_vsp_jpg_data = {
	.name = "sprd-jpg",
	.ranges = sprd_vsp_jpg_ranges,
	.num_ranges = ARRAY_SIZE(sprd_vsp_jpg_ranges),
};

static const struct of_device_id sprd_vsp_of_match[] = {
	{ .compatible = "sprd,ums9230-vsp", .data = &sprd_vsp_vsp_data },
	{ .compatible = "sprd,ums9230-jpg", .data = &sprd_vsp_jpg_data },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_vsp_of_match);

static struct platform_driver sprd_vsp_driver = {
	.probe = sprd_vsp_probe,
	.remove = sprd_vsp_remove,
	.driver = {
		.name = "sprd-vsp",
		.of_match_table = sprd_vsp_of_match,
		.pm = &sprd_vsp_pm_ops,
	},
};
module_platform_driver(sprd_vsp_driver);

MODULE_DESCRIPTION("Unisoc UMS9230 video and JPEG codec bring-up driver");
MODULE_LICENSE("GPL");
