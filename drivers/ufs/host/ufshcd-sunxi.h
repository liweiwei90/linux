/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2024 - 2026 Allwinner Technology Co.,Ltd. All rights reserved. */
/*
 * Driver for sunxi SD/MMC host controllers
 * Copyright (C) 2024-2026 lixiang <lixiang@allwinnertech>
 */

#ifndef _UFSHCD_SUNXI_H
#define _UFSHCD_SUNXI_H

#include "ufshcd-dwc.h"

struct pair_addr {
	u16 addr;
	u16 value;
};

// int ufshcd_sunxi_link_startup_notify(struct ufs_hba *hba,
// 				 enum ufs_notify_change_status status);
// int ufshcd_sunxi_dme_set_attrs(struct ufs_hba *hba,
// 			     const struct ufshcd_dme_attr_val *v, int n);
int ufshcd_sunxi_dme_get_attrs(struct ufs_hba *hba,
			     const struct ufshcd_dme_attr_val *v, int n);

#endif /* End of Header */
