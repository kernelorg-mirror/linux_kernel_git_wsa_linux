// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2015 Renesas Electronics Europe Ltd.
 * Copyright 2026 Bootlin
 *
 * Based on highbank EDAC driver:
 *
 * Copyright 2011-2012 Calxeda, Inc.
 */

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/edac.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "edac_mc.h"
#include "edac_module.h"

#define DRV_NAME				"cadence_edac"

#define REG_BYTE_SZ				4
#define DDR_CTL(n)				((n) * REG_BYTE_SZ)

#define CDNS_DDR_STAT				DDR_CTL(0)
#define CDNS_DDR_STAT_DRAM_CLASS		GENMASK_U32(11, 8)
#define CDNS_DDR_STAT_DRAM_DDR2			BIT(2)
#define CDNS_DDR_STAT_GET_DRAM_CLASS(reg)	FIELD_GET(CDNS_DDR_STAT_DRAM_CLASS, reg)

#define CDNS_ECC_STAT				DDR_CTL(36)
#define CDNS_ECC_STAT_ENABLED			BIT(16)
#define CDNS_ECC_STAT_IS_ENABLED(reg)		FIELD_GET(CDNS_ECC_STAT_ENABLED, reg)
#define CDNS_ECC_STAT_FWC			BIT(24)

#define CDNS_ECC_XOR				DDR_CTL(37)
#define CDNS_ECC_XOR_CHECK_BITS			GENMASK_U32(13, 0)

#define CDNS_BUS_CTRL				DDR_CTL(54)
#define CDNS_BUS_CTRL_REDUC			BIT(1)

/* DDR Controller Error Registers */

#define CDNS_ECC_U_ERR_ADDR			DDR_CTL(38)
#define CDNS_ECC_U_ERR_STAT			DDR_CTL(39)

#define CDNS_ECC_C_ERR_ADDR			DDR_CTL(41)
#define CDNS_ECC_C_ERR_STAT			DDR_CTL(42)

#define CDNS_ECC_ERR_STAT_GET_SYNDROME(reg)	FIELD_GET(GENMASK(6, 0), reg)

#define CDNS_PORT_CMD_ERR_ADDR			DDR_CTL(61)
#define CDNS_PORT_CMD_ERR_TYPE			DDR_CTL(62)
#define CDNS_PORT_CMD_ERR_TYPE_GET(reg)		FIELD_GET(GENMASK_U32(10, 8), reg)

/* DDR Controller Interrupt Registers */

#define CDNS_ECC_INT_STAT			DDR_CTL(56)
#define CDNS_ECC_INT_STAT_CE			BIT(3)
#define CDNS_ECC_INT_STAT_MULTIPLE_CE		BIT(4)
#define CDNS_ECC_INT_STAT_UE			BIT(5)
#define CDNS_ECC_INT_STAT_MULTIPLE_UE		BIT(6)
#define CDNS_ECC_INT_EDAC			(CDNS_ECC_INT_STAT_CE | \
						 CDNS_ECC_INT_STAT_MULTIPLE_CE | \
						 CDNS_ECC_INT_STAT_UE | \
						 CDNS_ECC_INT_STAT_MULTIPLE_UE)

#define CDNS_ECC_INT_ACK			DDR_CTL(57)
#define CDNS_ECC_INT_ACK_MASK			GENMASK_U32(21, 0)

#define CDNS_ECC_INT_CTRL			DDR_CTL(58)
#define CDNS_ECC_INT_CTRL_MASK			GENMASK_U32(21, 0)
#define CDNS_ECC_INT_CTRL_MASK_ALL		BIT(22)
#define CDNS_ECC_INT_CTRL_UNMASK(i)		((~(i)) & CDNS_ECC_INT_CTRL_MASK)

struct mc_priv {
	int irq;
	void __iomem *io_base;
	struct dentry *debugfs;
	unsigned long pfn_base;
	bool ecc_enabled;
	spinlock_t lock;
	u16 xor_check_bits;
};

static irqreturn_t cdns_mc_err_handler(int irq, void *dev_id)
{
	struct mem_ctl_info *mci = dev_id;
	struct mc_priv *priv = mci->pvt_info;
	u32 status, err_addr, syndrome, reg;

	status = readl(priv->io_base + CDNS_ECC_INT_STAT);
	if (!status)
		return IRQ_NONE;

	/*
	 * Hardware is not exposing how many CE / UE occurred since last ACK in
	 * case of multiple errors. Always set the error count to 1.
	 */

	if (status & (CDNS_ECC_INT_STAT_UE | CDNS_ECC_INT_STAT_MULTIPLE_UE)) {
		reg = readl(priv->io_base + CDNS_ECC_U_ERR_STAT);
		syndrome = CDNS_ECC_ERR_STAT_GET_SYNDROME(reg);

		err_addr = readl(priv->io_base + CDNS_ECC_U_ERR_ADDR);

		edac_mc_handle_error(HW_EVENT_ERR_UNCORRECTED, mci, 1,
				     priv->pfn_base + PFN_DOWN(err_addr), err_addr & ~PAGE_MASK,
				     syndrome, 0, 0, -1, mci->ctl_name, "");
	}

	if (status & (CDNS_ECC_INT_STAT_CE | CDNS_ECC_INT_STAT_MULTIPLE_CE)) {
		reg = readl(priv->io_base + CDNS_ECC_C_ERR_STAT);
		syndrome = CDNS_ECC_ERR_STAT_GET_SYNDROME(reg);

		err_addr = readl(priv->io_base + CDNS_ECC_C_ERR_ADDR);

		edac_mc_handle_error(HW_EVENT_ERR_CORRECTED, mci, 1,
				     priv->pfn_base + PFN_DOWN(err_addr), err_addr & ~PAGE_MASK,
				     syndrome, 0, 0, -1, mci->ctl_name, "");
	}

	writel(status & CDNS_ECC_INT_ACK_MASK, priv->io_base + CDNS_ECC_INT_ACK);

	return IRQ_HANDLED;
}

#ifdef CONFIG_EDAC_DEBUG

static void cdns_rmw(struct mc_priv *priv, u32 reg, u32 mask, u32 val)
{
	u32 regval;

	regval = readl(priv->io_base + reg);
	regval &= ~mask;
	regval |= (val << __bf_shf(mask)) & mask;
	writel(regval, priv->io_base + reg);
}

static ssize_t cdns_force_ecc_error(struct file *file, const char __user *data, size_t count,
				    loff_t *ppos)
{
	struct device *dev = file->private_data;
	struct mem_ctl_info *mci = to_mci(dev);
	struct mc_priv *priv = mci->pvt_info;

	spin_lock(&priv->lock);

	cdns_rmw(priv, CDNS_ECC_XOR, CDNS_ECC_XOR_CHECK_BITS, priv->xor_check_bits);
	cdns_rmw(priv, CDNS_ECC_STAT, CDNS_ECC_STAT_FWC, 1);

	spin_unlock(&priv->lock);

	return count;
}

static const struct file_operations cdns_ecc_error_fops = {
	.open = simple_open,
	.write = cdns_force_ecc_error,
	.llseek = generic_file_llseek,
};

static void cdns_setup_debugfs(struct mem_ctl_info *mci)
{
	struct mc_priv *priv = mci->pvt_info;

	if (!priv->ecc_enabled)
		return;

	priv->debugfs = edac_debugfs_create_dir(DRV_NAME);
	if (!priv->debugfs) {
		dev_dbg(mci->pdev, "failed to create debugfs dir\n");
		return;
	}

	edac_debugfs_create_x16("bits", 0644, priv->debugfs, &priv->xor_check_bits);
	edac_debugfs_create_file("inject", 0200, priv->debugfs, &mci->dev, &cdns_ecc_error_fops);
}

static void cdns_remove_debugfs(struct mem_ctl_info *mci)
{
	struct mc_priv *priv = mci->pvt_info;

	if (!priv->ecc_enabled)
		return;

	edac_debugfs_remove_recursive(priv->debugfs);
}

#else

static void cdns_setup_debugfs(struct mem_ctl_info *mci) {}

static void cdns_remove_debugfs(struct mem_ctl_info *mci) {}

#endif

static int cdns_setup_irq(struct platform_device *pdev, struct mem_ctl_info *mci)
{
	struct mc_priv *priv = mci->pvt_info;
	int ret;

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0)
		return dev_err_probe(&pdev->dev, priv->irq, "unable to get irq\n");

	ret = devm_request_irq(&pdev->dev, priv->irq, cdns_mc_err_handler, 0, dev_name(&pdev->dev),
			       mci);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "unable to request irq %d\n", priv->irq);

	return 0;
}

static int cdns_setup_mc(struct platform_device *pdev, void __iomem *io_base)
{
	struct edac_mc_layer layers[2];
	struct mem_ctl_info *mci;
	resource_size_t mem_sz;
	struct dimm_info *dimm;
	struct device_node *np;
	struct mc_priv *priv;
	struct resource res;
	int ret;
	u32 reg;

	layers[0].type = EDAC_MC_LAYER_CHIP_SELECT;
	layers[0].size = 1;
	layers[0].is_virt_csrow = true;
	layers[1].type = EDAC_MC_LAYER_CHANNEL;
	layers[1].size = 1;
	layers[1].is_virt_csrow = false;

	np = of_find_node_by_name(NULL, "memory");
	if (!np)
		return dev_err_probe(&pdev->dev, -ENODEV, "unable to find memory node\n");

	ret = of_address_to_resource(np, 0, &res);

	of_node_put(np);

	if (ret)
		return ret;

	mem_sz = resource_size(&res);

	mci = edac_mc_alloc(0, ARRAY_SIZE(layers), layers, sizeof(struct mc_priv));
	if (!mci)
		return dev_err_probe(&pdev->dev, -ENOMEM, "unable to allocate edac mc\n");

	mci->pdev = &pdev->dev;
	priv = mci->pvt_info;

	priv->io_base = io_base;
	spin_lock_init(&priv->lock);
	priv->xor_check_bits = 0;
	priv->pfn_base = PFN_DOWN(res.start);
	platform_set_drvdata(pdev, mci);

	reg = readl(priv->io_base + CDNS_ECC_STAT);
	priv->ecc_enabled = CDNS_ECC_STAT_IS_ENABLED(reg);

	mci->edac_cap = priv->ecc_enabled ? EDAC_FLAG_SECDED : EDAC_FLAG_NONE;
	mci->mtype_cap = MEM_FLAG_DDR2 | MEM_FLAG_DDR3;
	mci->edac_ctl_cap = EDAC_FLAG_NONE | EDAC_FLAG_SECDED;
	mci->mod_name = pdev->dev.driver->name;
	mci->ctl_name = "cdns-ddr-ctrl";
	mci->dev_name = dev_name(&pdev->dev);
	mci->scrub_mode = SCRUB_SW_SRC;

	dimm = *mci->dimms;
	dimm->nr_pages = PFN_UP(mem_sz);
	dimm->grain = 4;
	dimm->edac_mode = priv->ecc_enabled ? EDAC_SECDED : EDAC_NONE;

	/* Check if half datapath feature of the controller is active. */
	reg = readl(priv->io_base + CDNS_BUS_CTRL);
	if (reg & CDNS_BUS_CTRL_REDUC)
		dimm->dtype = DEV_X8;
	else
		dimm->dtype = DEV_X16;

	strscpy(dimm->label, "Channel#0_DIMM#0", sizeof(dimm->label));

	reg = readl(priv->io_base + CDNS_DDR_STAT);
	if (CDNS_DDR_STAT_GET_DRAM_CLASS(reg) == CDNS_DDR_STAT_DRAM_DDR2)
		dimm->mtype = MEM_DDR2;
	else
		dimm->mtype = MEM_DDR3;

	ret = edac_mc_add_mc(mci);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "failed to add mc\n");
		goto free_edac_mc;
	}

	ret = cdns_setup_irq(pdev, mci);
	if (ret)
		goto del_edac_mc;

	cdns_setup_debugfs(mci);

	edac_mc_chipset_printk(mci, KERN_DEBUG, DRV_NAME, "ECC %s\n",
			       (!priv->ecc_enabled) ? "disabled" : "enabled");

	return 0;

del_edac_mc:
	edac_mc_del_mc(&pdev->dev);
free_edac_mc:
	edac_mc_free(mci);
	return ret;
}

static int cdns_mc_probe(struct platform_device *pdev)
{
	void __iomem *io_base;
	struct clk *clk;
	int ret;

	io_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(io_base))
		return dev_err_probe(&pdev->dev, PTR_ERR(io_base), "unable to map regs\n");

	clk = devm_clk_get_enabled(&pdev->dev, "ddrc");
	if (IS_ERR(clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(clk), "unable to get ddrc\n");

	clk = devm_clk_get_enabled(&pdev->dev, "pclk");
	if (IS_ERR(clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(clk), "unable to get pclk\n");

	writel(CDNS_ECC_INT_CTRL_MASK_ALL, io_base + CDNS_ECC_INT_CTRL);

	ret = cdns_setup_mc(pdev, io_base);
	if (ret)
		return ret;

	writel(CDNS_ECC_INT_CTRL_UNMASK(CDNS_ECC_INT_EDAC),
	       io_base + CDNS_ECC_INT_CTRL);

	return 0;
}

static void cdns_mc_remove(struct platform_device *pdev)
{
	struct mem_ctl_info *mci = platform_get_drvdata(pdev);
	struct mc_priv *priv = mci->pvt_info;

	writel(CDNS_ECC_INT_CTRL_MASK_ALL, priv->io_base + CDNS_ECC_INT_CTRL);

	/* Wait for any pending interrupt before releasing resources. */
	devm_free_irq(&pdev->dev, priv->irq, mci);

	cdns_remove_debugfs(mci);

	edac_mc_del_mc(&pdev->dev);
	edac_mc_free(mci);
}

static const struct of_device_id cdns_ddr_ctrl_of_match[] = {
	{ .compatible = "cdns,ddr-edac" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, cdns_ddr_ctrl_of_match);

static struct platform_driver cdns_mc_edac_driver = {
	.probe = cdns_mc_probe,
	.remove = cdns_mc_remove,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = cdns_ddr_ctrl_of_match,
	},
};

module_platform_driver(cdns_mc_edac_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Renesas Electronics Europe Ltd.");
MODULE_AUTHOR("Paul Louvel <paul.louvel@bootlin.com>");
MODULE_DESCRIPTION("EDAC driver for Cadence DDR controller");
