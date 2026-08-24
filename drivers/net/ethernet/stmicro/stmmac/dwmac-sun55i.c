// SPDX-License-Identifier: GPL-2.0-only
/*
 * dwmac-sun55i.c - Allwinner sun55i GMAC200 specific glue layer
 *
 * Copyright (C) 2025 Chen-Yu Tsai <wens@csie.org>
 *
 * syscon parts taken from dwmac-sun8i.c, which is
 *
 * Copyright (C) 2017 Corentin Labbe <clabbe.montjoie@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/stmmac.h>

#include "stmmac.h"
#include "stmmac_platform.h"

#define SYSCON_REG		0x34

/* RMII specific bits */
#define SYSCON_RMII_EN		BIT(13) /* 1: enable RMII (overrides EPIT) */
/* Generic system control EMAC_CLK bits */
#define SYSCON_ETXDC_H_MASK		GENMASK(17, 16)
#define SYSCON_ETXDC_MASK		GENMASK(12, 10)
#define SYSCON_ERXDC_MASK		GENMASK(9, 5)
/* EMAC PHY Interface Type */
#define SYSCON_EPIT			BIT(2) /* 1: RGMII, 0: MII */
#define SYSCON_ETCS_MASK		GENMASK(1, 0)
#define SYSCON_ETCS_MII		0x0
#define SYSCON_ETCS_EXT_GMII	0x1
#define SYSCON_ETCS_INT_GMII	0x2

#define SYSCON_ETXDC_FULL_MASK		GENMASK(4, 0)

struct sun55i_priv_data {
	void __iomem *glue;
	u32 glue_setting;
};

typedef int (*gmac_plat_init)(struct device *dev,
			      struct plat_stmmacenet_data *plat,
			      struct stmmac_resources *res);

static int sun55i_gmac200_set_syscon(struct device *dev,
				     struct plat_stmmacenet_data *plat,
				     struct stmmac_resources *res)
{
	struct device_node *node = dev->of_node;
	struct regmap *regmap;
	u32 val, reg = 0;
	int ret;

	regmap = syscon_regmap_lookup_by_phandle(node, "syscon");
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "Unable to map syscon\n");

	if (!of_property_read_u32(node, "tx-internal-delay-ps", &val)) {
		if (val % 100)
			return dev_err_probe(dev, -EINVAL,
					     "tx-delay must be a multiple of 100ps\n");
		val /= 100;
		dev_dbg(dev, "set tx-delay to %x\n", val);
		if (!FIELD_FIT(SYSCON_ETXDC_FULL_MASK, val))
			return dev_err_probe(dev, -EINVAL,
					     "TX clock delay exceeds maximum (%u00ps > %lu00ps)\n",
					     val, FIELD_MAX(SYSCON_ETXDC_FULL_MASK));

		reg |= FIELD_PREP(SYSCON_ETXDC_MASK, val);
	}

	if (!of_property_read_u32(node, "rx-internal-delay-ps", &val)) {
		if (val % 100)
			return dev_err_probe(dev, -EINVAL,
					     "rx-delay must be a multiple of 100ps\n");
		val /= 100;
		dev_dbg(dev, "set rx-delay to %x\n", val);
		if (!FIELD_FIT(SYSCON_ERXDC_MASK, val))
			return dev_err_probe(dev, -EINVAL,
					     "RX clock delay exceeds maximum (%u00ps > %lu00ps)\n",
					     val, FIELD_MAX(SYSCON_ERXDC_MASK));

		reg |= FIELD_PREP(SYSCON_ERXDC_MASK, val);
	}

	switch (plat->phy_interface) {
	case PHY_INTERFACE_MODE_MII:
		/* default */
		break;
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		reg |= SYSCON_EPIT | SYSCON_ETCS_INT_GMII;
		break;
	case PHY_INTERFACE_MODE_RMII:
		reg |= SYSCON_RMII_EN;
		break;
	default:
		return dev_err_probe(dev, -EINVAL, "Unsupported interface mode: %s",
				     phy_modes(plat->phy_interface));
	}

	ret = regmap_write(regmap, SYSCON_REG, reg);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to write to syscon\n");

	return 0;
}

static struct mac_device_info *sun60i_dwmac_setup(void *opriv)
{
	struct stmmac_priv *stmmac_priv = opriv;
	struct sun55i_priv_data *priv = stmmac_priv->plat->bsp_priv;

	stmmac_priv->hw = devm_kzalloc(stmmac_priv->device,
				       sizeof(*stmmac_priv->hw), GFP_KERNEL);
	if (!stmmac_priv->hw)
		return NULL;

	writel(priv->glue_setting, priv->glue);
	udelay(100);

	dwmac4_setup(stmmac_priv);

	return stmmac_priv->hw;
}

static int sun60i_gmac210_plat_init(struct device *dev,
				    struct plat_stmmacenet_data *plat,
				    struct stmmac_resources *res)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct sun55i_priv_data *priv = plat->bsp_priv;
	struct device_node *node = dev->of_node;
	u32 val, reg = 0;

	priv->glue = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(priv->glue))
		return dev_err_probe(dev, PTR_ERR(priv->glue),
				     "Unable to map glue region\n");

	if (!of_property_read_u32(node, "tx-internal-delay-ps", &val)) {
		if (val % 100)
			return dev_err_probe(dev, -EINVAL,
					     "tx-delay must be a multiple of 100ps\n");
		val /= 100;
		dev_dbg(dev, "set tx-delay to %x\n", val);
		if (!FIELD_FIT(SYSCON_ETXDC_FULL_MASK, val))
			return dev_err_probe(dev, -EINVAL,
					     "TX clock delay exceeds maximum (%u00ps > %lu00ps)\n",
					     val, FIELD_MAX(SYSCON_ETXDC_MASK));

		reg |= FIELD_PREP(SYSCON_ETXDC_MASK,
				  FIELD_GET(GENMASK(2, 0), val));
		reg |= FIELD_PREP(SYSCON_ETXDC_H_MASK,
				  FIELD_GET(GENMASK(4, 3), val));
	}

	if (!of_property_read_u32(node, "rx-internal-delay-ps", &val)) {
		if (val % 100)
			return dev_err_probe(dev, -EINVAL,
					     "rx-delay must be a multiple of 100ps\n");
		val /= 100;
		dev_dbg(dev, "set rx-delay to %x\n", val);
		if (!FIELD_FIT(SYSCON_ERXDC_MASK, val))
			return dev_err_probe(dev, -EINVAL,
					     "RX clock delay exceeds maximum (%u00ps > %lu00ps)\n",
					     val, FIELD_MAX(SYSCON_ERXDC_MASK));

		reg |= FIELD_PREP(SYSCON_ERXDC_MASK, val);
	}

	switch (plat->phy_interface) {
	case PHY_INTERFACE_MODE_MII:
		/* default */
		break;
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		/*
		 * It seems the "internal GMAC transmit clock" isn't correctly
		 * supplied, and we must rely on the external RGMII clock
		 * supplied through RGMII0-CLKIN. Why?
		 */
		reg |= SYSCON_EPIT | SYSCON_ETCS_EXT_GMII;
		break;
	case PHY_INTERFACE_MODE_RMII:
		reg |= SYSCON_RMII_EN;
		break;
	default:
		return dev_err_probe(dev, -EINVAL, "Unsupported interface mode: %s",
				     phy_modes(plat->phy_interface));
	}

	priv->glue_setting = reg;

	res->tx_irq[0] = platform_get_irq_byname(pdev, "tx0_irq");
	if (res->tx_irq[0] < 0)
		return dev_err_probe(dev, res->tx_irq[0],
				     "Unable to request tx0_irq\n");

	res->rx_irq[0] = platform_get_irq_byname(pdev, "rx0_irq");
	if (res->rx_irq[0] < 0)
		return dev_err_probe(dev, res->rx_irq[0],
				     "Unable to request rx0_irq\n");

	plat->flags |= STMMAC_FLAG_MULTI_MSI_EN;
	plat->setup = sun60i_dwmac_setup;

	return 0;
}


static int sun55i_gmac200_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct device *dev = &pdev->dev;
	struct sun55i_priv_data *priv;
	gmac_plat_init init;
	struct clk *clk;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return ret;

	plat_dat = devm_stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return PTR_ERR(plat_dat);

	plat_dat->bsp_priv = priv;

	/* BSP disables it */
	plat_dat->flags |= STMMAC_FLAG_SPH_DISABLE;
	plat_dat->host_dma_width = 32;

	clk = devm_clk_get_optional_enabled(dev, "mbus");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "Failed to get or enable mbus clock\n");

	init = of_device_get_match_data(dev);
	ret = init(dev, plat_dat, &stmmac_res);
	if (ret)
		return ret;

	clk = devm_clk_get_optional_enabled(dev, "phy");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "Failed to get or enable phy clock\n");

	ret = devm_regulator_get_enable_optional(dev, "phy");
	if (ret && ret != -ENODEV)
		return dev_err_probe(dev, ret, "Failed to get or enable PHY supply\n");

	return devm_stmmac_pltfr_probe(pdev, plat_dat, &stmmac_res);
}

static const struct of_device_id sun55i_gmac200_match[] = {
	{
		.compatible	= "allwinner,sun55i-a523-gmac200",
		.data		= sun55i_gmac200_set_syscon,
	}, {
		.compatible	= "allwinner,sun60i-a733-gmac210",
		.data		= sun60i_gmac210_plat_init,
	}, { }
};
MODULE_DEVICE_TABLE(of, sun55i_gmac200_match);

static struct platform_driver sun55i_gmac200_driver = {
	.probe  = sun55i_gmac200_probe,
	.driver = {
		.name           = "dwmac-sun55i",
		.pm		= &stmmac_pltfr_pm_ops,
		.of_match_table = sun55i_gmac200_match,
	},
};
module_platform_driver(sun55i_gmac200_driver);

MODULE_AUTHOR("Chen-Yu Tsai <wens@csie.org>");
MODULE_DESCRIPTION("Allwinner sun55i GMAC200 specific glue layer");
MODULE_LICENSE("GPL");
