/*
 * arch/arm/mach-tegra/include/mach/powergate.h
 *
 * Copyright (c) 2010 Google, Inc
 *
 * Author:
 *	Colin Cross <ccross@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef _MACH_TEGRA_POWERGATE_H_
#define _MACH_TEGRA_POWERGATE_H_

struct clk;

#define TEGRA_POWERGATE_CPU	0
#define TEGRA_POWERGATE_3D	1
#define TEGRA_POWERGATE_VENC	2
#define TEGRA_POWERGATE_PCIE	3
#define TEGRA_POWERGATE_VDEC	4
#define TEGRA_POWERGATE_L2	5
#define TEGRA_POWERGATE_MPE	6
#define TEGRA_POWERGATE_HEG	7
#define TEGRA_POWERGATE_SATA	8
#define TEGRA_POWERGATE_CPU1	9
#define TEGRA_POWERGATE_CPU2	10
#define TEGRA_POWERGATE_CPU3	11
#define TEGRA_POWERGATE_CELP	12
#define TEGRA_POWERGATE_3D1	13

#define TEGRA_POWERGATE_CPU0	TEGRA_POWERGATE_CPU
#define TEGRA_POWERGATE_3D0	TEGRA_POWERGATE_3D

int  __init tegra_powergate_init(void);

int tegra_cpu_powergate_id(int cpuid);
int tegra_powergate_is_powered(int id);
int tegra_powergate_power_on(int id);
int tegra_powergate_power_off(int id);
int tegra_powergate_remove_clamping(int id);

int tegra_powergate_mc_disable(int id);
int tegra_powergate_mc_enable(int id);
int tegra_powergate_mc_flush(int id);
int tegra_powergate_mc_flush_done(int id);

/*
 * Functions to powergate un-powergate partitions.
 * Drivers are responsible for clk enable-disable
 *
 * tegra_powergate_partition() should be called with all
 * required clks OFF. Drivers should disable clks BEFORE
 * calling this fucntion
 *
 * tegra_unpowergate_partition should be called with all
 * required clks OFF. Returns with all clks OFF. Drivers
 * should enable all clks AFTER this function
 */
int tegra_powergate_partition(int id);
int tegra_unpowergate_partition(int id);

/* Must be called with clk disabled, and returns with clk enabled */
int tegra_powergate_sequence_power_up(int id, struct clk *clk);

#endif /* _MACH_TEGRA_POWERGATE_H_ */
