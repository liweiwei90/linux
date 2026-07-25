// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
/*
 * Allwinner pulse-width-modulation controller driver
 */

#include <linux/types.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/pwm.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/of_device.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/spinlock.h>

#define PWM_NUM_MAX 16
#define PWM_CHAN_NUM 10
#define PWM_BIND_NUM 2
#define PWM_PIN_STATE_ACTIVE "active"
#define PWM_PIN_STATE_SLEEP "sleep"
#define SUNXI_PWM_BIND_DEFAULT 255

#define SUNXI_PWM_NORMAL	1
#define SUNXI_PWM_INVERSED	0
#define SUNXI_PWM_SINGLE	1
#define SUNXI_PWM_DUAL		2
#define SUNXI_CLK_24M		24000000
#define SUNXI_CLK_100M		100000000
#define SUNXI_DIV_CLK		1000000000

#define PRESCALE_MAX 256

#define PWM_PCGR          0x0040
#define PWM_PDZCR01       0x0060
#define PWM_PDZCR23       0x0064
#define PWM_PDZCR45       0x0068
#define PWM_PDZCR67       0x006c
#define PWM_PDZCR89       0x0070
#define PWM_PDZCRAB       0x0074
#define PWM_PDZCRCD       0x0078
#define PWM_PDZCREF       0x007c
#define PWM_PGR0          0x0090

#define PWM_DIV_M_SHIFT          0
#define PWM_DIV_M_WIDTH          4
#define PWM_CLK_SRC_SHIFT        7
#define PWM_CLK_SRC_WIDTH        2
#define PWM_CLK_GATING_SHIFT     6
#define PWM_CLK_GATING_WIDTH     1
#define PWM_BYPASS_SHIFT         5
#define PWM_BYPASS_WIDTH         1
#define PWM_CGR_BYPASS_SHIFT     16

#define PWM_PRESCAL_SHIFT        0
#define PWM_PRESCAL_WIDTH        8
#define PWM_ACT_STA_SHIFT        5
#define PWM_ACT_STA_WIDTH        1
#define PWM_MODE_ACTS_SHIFT      8
#define PWM_MODE_ACTS_WIDTH      2
#define PWM_PUL_NUM_SHIFT        10
#define PWM_PUL_NUM_WIDTH        16

#define PWM_ACT_CYCLES_SHIFT     0
#define PWM_ACT_CYCLES_WIDTH     16
#define PWM_PERIOD_CYCLES_SHIFT  16
#define PWM_PERIOD_CYCLES_WIDTH  16

#define PWM_DZ_EN_SHIFT          0
#define PWM_DZ_EN_WIDTH          1
#define PWM_PDZINTV_SHIFT        8
#define PWM_PDZINTV_WIDTH        8

struct sunxi_pwm_config {
	unsigned int dead_time;
	unsigned int bind_pwm;
	bool clk_bypass_osc24m;
};

struct group_pwm_config {
	unsigned int group_channel;
	unsigned int group_run_count;
	unsigned int pwm_polarity;
	int pwm_period;
};

struct sunxi_pwm_hw_data {
	u32 pdzcr01_offset;
	u32 per_offset;
	u32 pcr_base_offset;
	u32 ppr_base_offset;
	u32 pcntr_base_offset;
	u32 ccr_base_offset;
	u32 pwm_reg_uniform_offset;
	bool clk_gating_separate;
	bool has_bus_clock;         /* v203: false, v204: true */
};

struct sunxi_pwm_chip {
	const struct sunxi_pwm_hw_data *data;
	void __iomem *base;

	struct sunxi_pwm_config *config;
	struct group_pwm_config *group_config;
	struct clk *clk;
	struct clk *bclk;
	struct reset_control *reset;
	unsigned int group_ch;
	unsigned int group_polarity;
	unsigned int group_period;
	struct pinctrl *pctl;
	bool channel_polarity_flag[PWM_NUM_MAX];

	u32 pm_regs_backup[15];
	u32 pcr_regs_backup[PWM_CHAN_NUM];
	u32 ppr_regs_backup[PWM_CHAN_NUM];
	u32 ccr_regs_backup[PWM_CHAN_NUM];
	u32 pcntr_regs_backup[PWM_CHAN_NUM];
	spinlock_t lock;
};

static const struct sunxi_pwm_hw_data sunxi_pwm_v203_data = {
	.pdzcr01_offset = 0x0060,
	.per_offset = 0x0080,
	.pcr_base_offset = 0x0100,
	.ppr_base_offset = 0x0104,
	.pcntr_base_offset = 0x0108,
	.ccr_base_offset = 0x0110,
	.clk_gating_separate = 1,
	.pwm_reg_uniform_offset = 64,
	.has_bus_clock = false,
};

static const struct sunxi_pwm_hw_data sunxi_pwm_v204_data = {
	.pdzcr01_offset = 0x0060,
	.per_offset = 0x0080,
	.pcr_base_offset = 0x0100,
	.ppr_base_offset = 0x0104,
	.pcntr_base_offset = 0x0108,
	.ccr_base_offset = 0x0110,
	.clk_gating_separate = 1,
	.pwm_reg_uniform_offset = 64,
	.has_bus_clock = true,
};

static int sunxi_pwm_regs[] = {
	0x0000, 0x0004, 0x0008, 0x000c, 0x0010, 0x0014, 0x0018, 0x001c
};

static u32 sunxi_pwm_pre_scal[][2] = {
	{0, 1}, {1, 2}, {2, 4}, {3, 8}, {4, 16}, {5, 32}, {6, 64}, {7, 128}, {8, 256},
};

static inline struct sunxi_pwm_chip *to_sunxi_pwm_chip(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

#define SETMASK(width, shift)   ((width?((-1U) >> (32-width)):0)  << (shift))
#define CLRMASK(width, shift)   (~(SETMASK(width, shift)))
#define GET_BITS(shift, width, reg)     (((reg) & SETMASK(width, shift)) >> (shift))
#define SET_BITS(shift, width, reg, val) (((reg) & CLRMASK(width, shift)) | (val << (shift)))

static void sunxi_pwm_set_reg(struct sunxi_pwm_chip *chip, u32 reg_offset, u32 reg_shift, u32 reg_width, int data)
{
	u32 value = readl(chip->base + reg_offset);
	value = SET_BITS(reg_shift, reg_width, value, data);
	writel(value, chip->base + reg_offset);
}

static int get_pdzcr_reg_offset(struct sunxi_pwm_chip *chip, u32 sel, u32 *reg_offset)
{
	if (sel <= 1)       *reg_offset = chip->data->pdzcr01_offset;
	else if (sel <= 3)  *reg_offset = PWM_PDZCR23;
	else if (sel <= 5)  *reg_offset = PWM_PDZCR45;
	else if (sel <= 7)  *reg_offset = PWM_PDZCR67;
	else if (sel <= 9)  *reg_offset = PWM_PDZCR89;
	else if (sel <= 11) *reg_offset = PWM_PDZCRAB;
	else if (sel <= 13) *reg_offset = PWM_PDZCRCD;
	else if (sel <= 15) *reg_offset = PWM_PDZCREF;
	else return -EINVAL;
	return 0;
}

static long sunxi_pwm_config_clk(struct sunxi_pwm_chip *chip, u32 hwpwm, int period_ns, int mode_num)
{
	int src_clk_sel, i, bind_num;
	bool clk_bypass_osc24m = chip->config[hwpwm].clk_bypass_osc24m;
	unsigned int index[2] = { hwpwm, 0 };
	unsigned int reg_bypass_shift, reg_offset[2];
	unsigned long long clk = 0;
	unsigned long flags;

	bind_num = chip->config[hwpwm].bind_pwm;
	reg_offset[0] = sunxi_pwm_regs[index[0] >> 1];
	if (mode_num == SUNXI_PWM_DUAL) {
		index[1] = bind_num;
		reg_offset[1] = sunxi_pwm_regs[index[1] >> 1];
	}

	if (clk_bypass_osc24m) {
		clk = SUNXI_CLK_24M;
		src_clk_sel = 0;
		if (!chip->data->clk_gating_separate) {
			reg_bypass_shift = (index[0] % 2 == 0) ? PWM_BYPASS_SHIFT : (PWM_BYPASS_SHIFT + 1);
			sunxi_pwm_set_reg(chip, reg_offset[0], reg_bypass_shift, PWM_BYPASS_WIDTH, 1);
		} else {
			reg_bypass_shift = index[0] + PWM_CGR_BYPASS_SHIFT;
			sunxi_pwm_set_reg(chip, PWM_PCGR, reg_bypass_shift, PWM_BYPASS_WIDTH, 1);
		}
	} else if (period_ns > 0 && period_ns <= 10) {
		src_clk_sel = 1;
		spin_lock_irqsave(&chip->lock, flags);
		for (i = 0; i < mode_num; i++) {
			if (!chip->data->clk_gating_separate) {
				reg_bypass_shift = (index[i] % 2 == 0) ? PWM_BYPASS_SHIFT : (PWM_BYPASS_SHIFT + 1);
				sunxi_pwm_set_reg(chip, reg_offset[i], reg_bypass_shift, PWM_BYPASS_WIDTH, 1);
			} else {
				reg_bypass_shift = index[i] + PWM_CGR_BYPASS_SHIFT;
				sunxi_pwm_set_reg(chip, PWM_PCGR, reg_bypass_shift, PWM_BYPASS_WIDTH, 1);
			}
			sunxi_pwm_set_reg(chip, reg_offset[i], PWM_CLK_SRC_SHIFT, PWM_CLK_SRC_WIDTH, src_clk_sel);
		}
		spin_unlock_irqrestore(&chip->lock, flags);
		return 0;
	} else if (period_ns > 10 && period_ns <= 334) {
		clk = SUNXI_CLK_100M;
		src_clk_sel = 1;
	} else if (period_ns > 334) {
		clk = SUNXI_CLK_24M;
		src_clk_sel = 0;
	} else {
		return -EINVAL;
	}

	for (i = 0; i < mode_num; i++) {
		reg_offset[i] = sunxi_pwm_regs[index[i] >> 1];
		sunxi_pwm_set_reg(chip, reg_offset[i], PWM_CLK_SRC_SHIFT, PWM_CLK_SRC_WIDTH, src_clk_sel);
	}

	return clk;
}

static int sunxi_pwm_config_single(struct sunxi_pwm_chip *chip, u32 sel, int duty_ns, int period_ns)
{
	unsigned int temp, reg_offset, value;
	unsigned long long c = 0;
	unsigned long entire_cycles = 256, active_cycles = 192;
	unsigned int pre_scal_id = 0, div_m = 0, prescale = 0;
	unsigned int pwm_run_count = 0;
	struct group_pwm_config *pdevice = &chip->group_config[sel];
	unsigned long flags;

	if (pdevice && pdevice->group_channel) {
		pwm_run_count = pdevice->group_run_count;
		chip->group_ch = pdevice->group_channel;
		chip->group_polarity = pdevice->pwm_polarity;
		chip->group_period = pdevice->pwm_period;
	}

	if (chip->group_ch) {
		spin_lock_irqsave(&chip->lock, flags);
		reg_offset = chip->data->per_offset;
		value = readl(chip->base + reg_offset);
		value &= ~((0xf) << 4 * (chip->group_ch - 1));
		writel(value, chip->base + reg_offset);
		spin_unlock_irqrestore(&chip->lock, flags);
	}

	reg_offset = sunxi_pwm_regs[sel >> 1];
	writel(0, chip->base + reg_offset);

	if (chip->group_ch) {
		sunxi_pwm_set_reg(chip, reg_offset, PWM_CLK_SRC_SHIFT, PWM_CLK_SRC_WIDTH, 0);
	} else {
		c = sunxi_pwm_config_clk(chip, sel, period_ns, SUNXI_PWM_SINGLE);
		if (c <= 0) return c;
		c = c * period_ns;
		do_div(c, SUNXI_DIV_CLK);
		entire_cycles = (unsigned long)c;

		for (pre_scal_id = 0; pre_scal_id < 9; pre_scal_id++) {
			if (entire_cycles <= 65536) break;
			for (prescale = 0; prescale < PRESCALE_MAX + 1; prescale++) {
				entire_cycles = ((unsigned long)c / sunxi_pwm_pre_scal[pre_scal_id][1]) / (prescale + 1);
				if (entire_cycles <= 65536) {
					div_m = sunxi_pwm_pre_scal[pre_scal_id][0];
					break;
				}
			}
		}
		c = (unsigned long long)entire_cycles * duty_ns;
		do_div(c, period_ns);
		active_cycles = c;
		if (entire_cycles == 0) entire_cycles++;
	}

	temp = readl(chip->base + reg_offset);
	temp = SET_BITS(PWM_DIV_M_SHIFT, PWM_DIV_M_WIDTH, temp, chip->group_ch ? 0 : div_m);
	writel(temp, chip->base + reg_offset);

	reg_offset = chip->data->pcr_base_offset + chip->data->pwm_reg_uniform_offset * sel;
	temp = readl(chip->base + reg_offset);
	temp = SET_BITS(PWM_PRESCAL_SHIFT, PWM_PRESCAL_WIDTH, temp, chip->group_ch ? 0xef : prescale);
	writel(temp, chip->base + reg_offset);

	if (chip->group_ch) {
		reg_offset = PWM_PGR0 + 0x04 * (chip->group_ch - 1);
		sunxi_pwm_set_reg(chip, reg_offset, sel, 1, 1);

		reg_offset = chip->data->pcr_base_offset + sel * chip->data->pwm_reg_uniform_offset;
		temp = readl(chip->base + reg_offset);
		temp = SET_BITS(PWM_MODE_ACTS_SHIFT, PWM_MODE_ACTS_WIDTH, temp, 0x3);
		temp = SET_BITS(PWM_PUL_NUM_SHIFT, PWM_PUL_NUM_WIDTH, temp, pwm_run_count);
		writel(temp, chip->base + reg_offset);
	}

	reg_offset = chip->data->ppr_base_offset + chip->data->pwm_reg_uniform_offset * sel;
	temp = readl(chip->base + reg_offset);
	if (chip->group_ch) {
		temp = SET_BITS(PWM_ACT_CYCLES_SHIFT, PWM_ACT_CYCLES_WIDTH, temp, (unsigned int)((chip->group_period * 3) >> 3));
		temp = SET_BITS(PWM_PERIOD_CYCLES_SHIFT, PWM_PERIOD_CYCLES_WIDTH, temp, chip->group_period);
		chip->group_ch = 0;
	} else {
		temp = SET_BITS(PWM_ACT_CYCLES_SHIFT, PWM_ACT_CYCLES_WIDTH, temp, active_cycles);
		temp = SET_BITS(PWM_PERIOD_CYCLES_SHIFT, PWM_PERIOD_CYCLES_WIDTH, temp, (entire_cycles - 1));
	}
	writel(temp, chip->base + reg_offset);

	return 0;
}

static int sunxi_pwm_config_dual(struct sunxi_pwm_chip *chip, u32 sel, int duty_ns, int period_ns, int bind_num)
{
	unsigned int temp, reg_offset[2], reg_dz_en_offset;
	unsigned long long c = 0, clk = 0, clk_temp = 0;
	unsigned long reg_dead, entire_cycles = 256, active_cycles = 192;
	unsigned int pre_scal_id = 0, div_m = 0, prescale = 0;
	int pwm_index[2] = { (int)sel, bind_num };
	int i, err;
	unsigned int dead_time = chip->config[sel].dead_time;

	err = get_pdzcr_reg_offset(chip, sel, &reg_dz_en_offset);
	if (err) return -EINVAL;

	sunxi_pwm_set_reg(chip, reg_dz_en_offset, PWM_DZ_EN_SHIFT, PWM_DZ_EN_WIDTH, 1);
	if ((readl(chip->base + reg_dz_en_offset) & (1u << PWM_DZ_EN_SHIFT)) == 0) return -EINVAL;

	if ((unsigned int)duty_ns < dead_time) {
		if (duty_ns) return -EINVAL;
		duty_ns = dead_time * 10;
		if (duty_ns > period_ns) duty_ns = period_ns;
	}

	for (i = 0; i < PWM_BIND_NUM; i++) {
		reg_offset[i] = sunxi_pwm_regs[pwm_index[i] >> 1];
		writel(0, chip->base + reg_offset[i]);
	}

	clk = sunxi_pwm_config_clk(chip, sel, period_ns, SUNXI_PWM_DUAL);
	if (clk <= 0) return clk;

	c = clk * period_ns;
	do_div(c, SUNXI_DIV_CLK);
	entire_cycles = (unsigned long)c;

	clk_temp = clk * dead_time;
	do_div(clk_temp, SUNXI_DIV_CLK);
	reg_dead = (unsigned long)clk_temp;

	for (pre_scal_id = 0; pre_scal_id < 9; pre_scal_id++) {
		if (entire_cycles <= 65536 && reg_dead <= 256) break;
		for (prescale = 0; prescale < PRESCALE_MAX + 1; prescale++) {
			entire_cycles = ((unsigned long)c / sunxi_pwm_pre_scal[pre_scal_id][1]) / (prescale + 1);
			unsigned long long tmp = clk * dead_time;
			do_div(tmp, (unsigned long long)sunxi_pwm_pre_scal[pre_scal_id][1] * (prescale + 1));
			reg_dead = (unsigned long)tmp;
			if (entire_cycles <= 65536 && reg_dead <= 256) {
				div_m = sunxi_pwm_pre_scal[pre_scal_id][0];
				break;
			}
		}
	}

	c = (unsigned long long)entire_cycles * duty_ns;
	do_div(c, period_ns);
	active_cycles = c;
	if (entire_cycles == 0) entire_cycles++;

	for (i = 0; i < PWM_BIND_NUM; i++)
		sunxi_pwm_set_reg(chip, reg_offset[i], PWM_DIV_M_SHIFT, PWM_DIV_M_WIDTH, div_m);

	for (i = 0; i < PWM_BIND_NUM; i++) {
		reg_offset[i] = chip->data->pcr_base_offset + chip->data->pwm_reg_uniform_offset * pwm_index[i];
		sunxi_pwm_set_reg(chip, reg_offset[i], PWM_PRESCAL_SHIFT, PWM_PRESCAL_WIDTH, prescale);
	}

	for (i = 0; i < PWM_BIND_NUM; i++) {
		reg_offset[i] = chip->data->ppr_base_offset + chip->data->pwm_reg_uniform_offset * pwm_index[i];
		temp = readl(chip->base + reg_offset[i]);
		temp = SET_BITS(PWM_ACT_CYCLES_SHIFT, PWM_ACT_CYCLES_WIDTH, temp, active_cycles);
		temp = SET_BITS(PWM_PERIOD_CYCLES_SHIFT, PWM_PERIOD_CYCLES_WIDTH, temp, (entire_cycles - 1));
		writel(temp, chip->base + reg_offset[i]);
	}

	sunxi_pwm_set_reg(chip, reg_dz_en_offset, PWM_PDZINTV_SHIFT, PWM_PDZINTV_WIDTH, (unsigned int)reg_dead);
	return 0;
}

static int sunxi_pwm_hw_enable(struct sunxi_pwm_chip *chip, u32 index, bool enable)
{
	unsigned int reg_offset, reg_shift, reg_width;
	int bind_num = chip->config[index].bind_pwm;
	int mode_num = (bind_num == SUNXI_PWM_BIND_DEFAULT) ? SUNXI_PWM_SINGLE : SUNXI_PWM_DUAL;
	int idx_list[2] = { (int)index, bind_num };
	int i;
	unsigned long flags;

	struct group_pwm_config *pdevice = &chip->group_config[index];
	if (pdevice && pdevice->group_channel) chip->group_ch = pdevice->group_channel;

	spin_lock_irqsave(&chip->lock, flags);
	for (i = 0; i < mode_num; i++) {
		int curr_idx = idx_list[i];
		if (!chip->data->clk_gating_separate) {
			reg_offset = sunxi_pwm_regs[curr_idx >> 1];
			reg_shift = PWM_CLK_GATING_SHIFT;
			reg_width = PWM_CLK_GATING_WIDTH;
		} else {
			reg_offset = PWM_PCGR;
			reg_shift = curr_idx;
			reg_width = 1;
		}
		sunxi_pwm_set_reg(chip, reg_offset, reg_shift, reg_width, enable ? 1 : 0);

		reg_offset = chip->data->per_offset;
		if (chip->group_ch) {
			reg_shift = 4 * (chip->group_ch - 1) + 2;
			reg_width = 1;
		} else {
			reg_shift = curr_idx;
			reg_width = 1;
		}
		sunxi_pwm_set_reg(chip, reg_offset, reg_shift, reg_width, enable ? 1 : 0);
	}
	if (!enable) chip->group_ch = 0;
	spin_unlock_irqrestore(&chip->lock, flags);

	return 0;
}

static int sunxi_pwm_hw_set_polarity(struct sunxi_pwm_chip *chip, u32 index, enum pwm_polarity polarity)
{
	int bind_num = chip->config[index].bind_pwm;
	int mode_num = (bind_num == SUNXI_PWM_BIND_DEFAULT) ? SUNXI_PWM_SINGLE : SUNXI_PWM_DUAL;
	int idx_list[2] = { (int)index, bind_num };
	u32 temp[2], reg_offset[2];
	int i;
	unsigned long flags;

	for (i = 0; i < mode_num; i++) {
		reg_offset[i] = chip->data->pcr_base_offset + idx_list[i] * chip->data->pwm_reg_uniform_offset;
		temp[i] = readl(chip->base + reg_offset[i]);
	}

	spin_lock_irqsave(&chip->lock, flags);
	if (polarity == PWM_POLARITY_NORMAL)
		temp[0] = SET_BITS(PWM_ACT_STA_SHIFT, PWM_ACT_STA_WIDTH, temp[0], SUNXI_PWM_NORMAL);
	else
		temp[0] = SET_BITS(PWM_ACT_STA_SHIFT, PWM_ACT_STA_WIDTH, temp[0], SUNXI_PWM_INVERSED);

	if (mode_num == SUNXI_PWM_DUAL) {
		if (polarity == PWM_POLARITY_NORMAL)
			temp[1] = SET_BITS(PWM_ACT_STA_SHIFT, PWM_ACT_STA_WIDTH, temp[1], SUNXI_PWM_INVERSED);
		else
			temp[1] = SET_BITS(PWM_ACT_STA_SHIFT, PWM_ACT_STA_WIDTH, temp[1], SUNXI_PWM_NORMAL);
	}

	for (i = 0; i < mode_num; i++)
		writel(temp[i], chip->base + reg_offset[i]);
	spin_unlock_irqrestore(&chip->lock, flags);

	return 0;
}

static int sunxi_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm, const struct pwm_state *state)
{
	struct sunxi_pwm_chip *sunxi_chip = to_sunxi_pwm_chip(chip);
	u32 hwpwm = pwm->hwpwm;
	int bind_num = sunxi_chip->config[hwpwm].bind_pwm;
	int ret;

	if (!state->enabled) {
		sunxi_pwm_hw_enable(sunxi_chip, hwpwm, false);
		return 0;
	}

	ret = sunxi_pwm_hw_set_polarity(sunxi_chip, hwpwm, state->polarity);
	if (ret) return ret;

	if (bind_num == SUNXI_PWM_BIND_DEFAULT)
		ret = sunxi_pwm_config_single(sunxi_chip, hwpwm, state->duty_cycle, state->period);
	else
		ret = sunxi_pwm_config_dual(sunxi_chip, hwpwm, state->duty_cycle, state->period, bind_num);
	if (ret) return ret;

	return sunxi_pwm_hw_enable(sunxi_chip, hwpwm, true);
}

static int sunxi_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm, struct pwm_state *state)
{
	struct sunxi_pwm_chip *sunxi_chip = to_sunxi_pwm_chip(chip);
	u32 hwpwm = pwm->hwpwm;
	u32 reg_pcr = sunxi_chip->data->pcr_base_offset + hwpwm * sunxi_chip->data->pwm_reg_uniform_offset;
	u32 reg_ppr = sunxi_chip->data->ppr_base_offset + hwpwm * sunxi_chip->data->pwm_reg_uniform_offset;
	u32 pcr = readl(sunxi_chip->base + reg_pcr);
	u32 ppr = readl(sunxi_chip->base + reg_ppr);
	u32 entire, active;

	state->enabled = !!(readl(sunxi_chip->base + sunxi_chip->data->per_offset) & (1 << hwpwm));
	state->polarity = (GET_BITS(PWM_ACT_STA_SHIFT, PWM_ACT_STA_WIDTH, pcr) == SUNXI_PWM_NORMAL) ? 
	                  PWM_POLARITY_NORMAL : PWM_POLARITY_INVERSED;

	entire = GET_BITS(PWM_PERIOD_CYCLES_SHIFT, PWM_PERIOD_CYCLES_WIDTH, ppr) + 1;
	active = GET_BITS(PWM_ACT_CYCLES_SHIFT, PWM_ACT_CYCLES_WIDTH, ppr);

	if (entire <= 1) {
		state->period = 0;
		state->duty_cycle = 0;
	} else {
		state->period = (u64)entire * SUNXI_DIV_CLK / SUNXI_CLK_24M;
		state->duty_cycle = (u64)active * state->period / entire;
	}
	return 0;
}

static const struct pwm_ops sunxi_pwm_ops = {
	.apply = sunxi_pwm_apply,
	.get_state = sunxi_pwm_get_state,
};

static int sunxi_pwm_parse_dt(struct sunxi_pwm_chip *chip, struct device *dev, u32 npwm)
{
	struct device_node *np = dev->of_node;
	int i;

	chip->config = devm_kcalloc(dev, npwm, sizeof(struct sunxi_pwm_config), GFP_KERNEL);
	chip->group_config = devm_kcalloc(dev, npwm, sizeof(struct group_pwm_config), GFP_KERNEL);
	if (!chip->config || !chip->group_config) return -ENOMEM;

	for (i = 0; i < npwm; i++) {
		chip->config[i].bind_pwm = SUNXI_PWM_BIND_DEFAULT;
		chip->config[i].dead_time = 0;
		chip->config[i].clk_bypass_osc24m = of_property_read_bool(np, "clk_bypass_osc24m");
	}

	of_property_read_u32(np, "bind_pwm", &chip->config[0].bind_pwm);
	of_property_read_u32(np, "dead_time", &chip->config[0].dead_time);
	of_property_read_u32(np, "group_channel", &chip->group_config[0].group_channel);
	of_property_read_u32(np, "group_run_count", &chip->group_config[0].group_run_count);

	return 0;
}

static int sunxi_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pwm_chip *pwm_chip;
	struct sunxi_pwm_chip *chip;
	u32 val_pwm_num = 0;
	int ret;

	if (of_property_read_u32(dev->of_node, "pwm-number", &val_pwm_num)) {
		if (of_property_read_u32(dev->of_node, "npwm", &val_pwm_num))
			return -EINVAL;
	}

	if (val_pwm_num > PWM_NUM_MAX) val_pwm_num = PWM_NUM_MAX;

	pwm_chip = devm_pwmchip_alloc(dev, val_pwm_num, sizeof(struct sunxi_pwm_chip));
	if (IS_ERR(pwm_chip)) return PTR_ERR(pwm_chip);

	chip = pwmchip_get_drvdata(pwm_chip);
	chip->data = of_device_get_match_data(dev);
	if (!chip->data) return -EINVAL;
	
	spin_lock_init(&chip->lock);
	platform_set_drvdata(pdev, chip);

	pwm_chip->ops = &sunxi_pwm_ops;

	chip->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(chip->base)) return PTR_ERR(chip->base);

	{
		chip->reset = devm_reset_control_get_optional_shared(dev, NULL);
		if (IS_ERR(chip->reset)) return PTR_ERR(chip->reset);
		reset_control_deassert(chip->reset);

		chip->clk = devm_clk_get(&pdev->dev, NULL);
		if (!chip->clk) {
			chip->clk = of_clk_get(pdev->dev.of_node, 0);
			if (IS_ERR_OR_NULL(chip->clk)) {
				dev_err(&pdev->dev, "fail to get pwm clk!\n");
				return -EINVAL;
			}
		}
		if (chip->data->has_bus_clock) {
			chip->bclk = devm_clk_get(&pdev->dev, "clk_bus_pwm");
			if (!chip->bclk) {
				dev_err(&pdev->dev, "fail to get pwm clk!\n");
				return -EINVAL;
			}
		}
	}

	ret = sunxi_pwm_parse_dt(chip, dev, val_pwm_num);
	if (ret) return ret;

	chip->pctl = devm_pinctrl_get_select(dev, PWM_PIN_STATE_ACTIVE);
	if (IS_ERR(chip->pctl)) {
		dev_dbg(dev, "pinctrl active state not specified, fallback to default\n");
	}

	ret = devm_pwmchip_add(dev, pwm_chip);
	if (ret < 0) return ret;

	return 0;
}

static void sunxi_pwm_remove(struct platform_device *pdev)
{
	struct sunxi_pwm_chip *chip = platform_get_drvdata(pdev);

	if (chip->bclk) clk_disable_unprepare(chip->bclk);
	clk_disable_unprepare(chip->clk);
	reset_control_assert(chip->reset);
}

static const struct of_device_id sunxi_pwm_match[] = {
	{ .compatible = "allwinner,sunxi-pwm-v203", .data = &sunxi_pwm_v203_data },
	{ .compatible = "allwinner,sunxi-pwm-v204", .data = &sunxi_pwm_v204_data },
	{ },
};
MODULE_DEVICE_TABLE(of, sunxi_pwm_match);

static struct platform_driver sunxi_pwm_driver = {
	.probe = sunxi_pwm_probe,
	.remove = sunxi_pwm_remove,
	.driver = {
		.name = "sunxi-pwm",
		.of_match_table = sunxi_pwm_match,
	},
};
module_platform_driver(sunxi_pwm_driver);

MODULE_DESCRIPTION("Allwinner A733 PWM Driver");
MODULE_LICENSE("GPL v2");
