/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2024 - 2026 Allwinner Technology Co.,Ltd. All rights reserved. */
/*
 * Driver for sunxi SD/MMC host controllers
 * Copyright (C) 2024-2026 lixiang <lixiang@allwinnertech>
 */

#ifndef _UFSHCI_SUNXI_H
#define _UFSHCI_SUNXI_H

#include "ufshci-dwc.h"

/* DWC HC UFSHCI specific Registers */
enum sunxi_ufs_specific_registers {
	/*sunxi ufs special reg*/
	REG_UFS_CFG 				= 0x200,
/* sunxi Host Controller cfg 0x200h */
#define MPHY_SRAM_INIT_DONE_BIT			(0x1U<<24)
#define MPHY_SRAM_BYPASS_BIT			(0x1U<<20)
#define MPHY_SRAM_EXT_LD_DONE_BIT		(0x1U<<19)
#define CFG_CLK_FREQ_MASK 			(0x3f<<8)
#define CFG_CLK_19_2M 				(0x13<<8)
//#define REF_CLK_EN_UNIPRO_BIT			(0x1U<<5)
#define REF_CLK_UNIPRO_SEL_BIT 			(0x1U<<5)
//#define REF_CLK_EN_APP_BIT			(0x1U<<4)
//unused now,use U3U2_PHY_MUX_CTRL[8] instead
#define REF_CLK_APP_SEL_BIT 			(0x1U<<4)
#define SUNXI_UFS_REF_CLK_EN_APP_EN 		(1U<<3)
#define REF_CLK_FREQ_SEL_MASK 			(0x3U)
#define REF_CLK_FREQ_SEL_19_2M 			(0x0U)
#define REF_CLK_FREQ_SEL_26M 			(0x1U)
#define REF_CLK_FREQ_SEL_38_4M 			(0x2U)
#define REF_CLK_FREQ_SEL_52M 			(0x3U)
	REG_UFS_CLK_GATE			= 0x204,
/*204*/
#define MPHY_CFGCLK_GATE_BIT 			(1U<<16)
#define CLK24M_GATE_BIT 			(1U<<17)
#define HW_RST_N 	 			(0x1U<<2)

	REG_UFS_PPU_ACTIVE 				= 0x32c,
#define PPU_ACTIVE_EN				(1U << 0)
};

#endif /* End of Header */
