/*
 * Copyright (C) 2011 Google, Inc.
 *
 * Author:
 *	Colin Cross <ccross@android.com>
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

#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/moduleparam.h>
#include <linux/seq_file.h>
#include <linux/syscore_ops.h>

#include <mach/pm-irq.h>

#include "iomap.h"
#include "wakeups.h"
#include "pm.h"

#define PMC_CTRL		0x0
#define PMC_CTRL_LATCH_WAKEUPS	(1 << 5)
#define PMC_WAKE_MASK		0xc
#define PMC_WAKE_LEVEL		0x10
#define PMC_WAKE_STATUS		0x14
#define PMC_SW_WAKE_STATUS	0x18

#define PMC_MAX_WAKE_COUNT 64

static void __iomem *pmc = IO_ADDRESS(TEGRA_PMC_BASE);

static u64 tegra_lp0_wake_enb;
static u64 tegra_lp0_wake_level;
static u64 tegra_lp0_wake_level_any;

static unsigned int tegra_wake_irq_count[PMC_MAX_WAKE_COUNT];

static bool debug_lp0;
module_param(debug_lp0, bool, S_IRUGO | S_IWUSR);

static bool warn_prevent_lp0;
module_param(warn_prevent_lp0, bool, S_IRUGO | S_IWUSR);

static inline void write_pmc_wake_mask(u64 value)
{
	pr_info("Wake[31-0] enable=0x%x\n", (u32)(value & 0xFFFFFFFF));
	writel((u32)value, pmc + PMC_WAKE_MASK);
}

static inline u64 read_pmc_wake_level(void)
{
	u64 reg = readl(pmc + PMC_WAKE_LEVEL);

	return reg;
}

static inline void write_pmc_wake_level(u64 value)
{
	pr_info("Wake[31-0] level=0x%x\n", (u32)(value & 0xFFFFFFFF));
	writel((u32)value, pmc + PMC_WAKE_LEVEL);
}

static inline u64 read_pmc_wake_status(void)
{
	u64 reg = readl(pmc + PMC_WAKE_STATUS);

	return reg;
}

static inline u64 read_pmc_sw_wake_status(void)
{
	u64 reg = readl(pmc + PMC_SW_WAKE_STATUS);

	return reg;
}

static inline void clear_pmc_sw_wake_status(void)
{
	pmc_32kwritel(0, PMC_SW_WAKE_STATUS);
}

int tegra_pm_irq_set_wake(int irq, int enable)
{
	struct wake_mask_types wake_msk;
	int flow_type = -1;
	int err;

	err = tegra_irq_to_wake(irq, flow_type, &wake_msk);
	if (err)
		return 0;

	if (enable)
		tegra_lp0_wake_enb |= (wake_msk.wake_mask_hi |
			wake_msk.wake_mask_lo | wake_msk.wake_mask_any);
	else
		tegra_lp0_wake_enb &= ~(wake_msk.wake_mask_hi |
			wake_msk.wake_mask_lo | wake_msk.wake_mask_any);

	return 0;
}

int tegra_pm_irq_set_wake_type(int irq, int flow_type)
{
	struct wake_mask_types wake_msk;
	int err;

	err = tegra_irq_to_wake(irq, flow_type, &wake_msk);
	if (err < 0)
		return 0;

	/* configure LOW/FALLING polarity wake sources for an irq */
	tegra_lp0_wake_level &= ~wake_msk.wake_mask_lo;
	tegra_lp0_wake_level_any &= ~wake_msk.wake_mask_lo;

	/* configure HIGH/RISING polarity wake sources for an irq */
	tegra_lp0_wake_level |= wake_msk.wake_mask_hi;
	tegra_lp0_wake_level_any &= ~wake_msk.wake_mask_hi;

	/*
	 * configure RISING and FALLING i.e. ANY polarity wake
	 * sources for an irq
	 */
	tegra_lp0_wake_level_any |= wake_msk.wake_mask_any;

	return 0;
}

/* translate lp0 wake sources back into irqs to catch edge triggered wakeups */
static void tegra_pm_irq_syscore_resume_helper(
	unsigned long wake_status,
	unsigned int index)
{
	int wake;
	int irq;
	struct irq_desc *desc;

	for_each_set_bit(wake, &wake_status, sizeof(wake_status) * 8) {
		irq = tegra_wake_to_irq(wake + 32 * index);
		if (!irq) {
			pr_info("Resume caused by WAKE%d\n",
				(wake + 32 * index));
			continue;
		}

		desc = irq_to_desc(irq);
		if (!desc || !desc->action || !desc->action->name) {
			pr_info("Resume caused by WAKE%d, irq %d\n",
				(wake + 32 * index), irq);
			continue;
		}

		pr_info("Resume caused by WAKE%d, %s\n", (wake + 32 * index),
			desc->action->name);

		tegra_wake_irq_count[wake + 32 * index]++;

		generic_handle_irq(irq);
	}
}

static void tegra_pm_irq_syscore_resume(void)
{
	unsigned long long wake_status = read_pmc_wake_status();

	pr_info("legacy wake status=0x%x\n", (u32)wake_status);
	tegra_pm_irq_syscore_resume_helper((unsigned long)wake_status, 0);
}

/* set up lp0 wake sources */
static int tegra_pm_irq_syscore_suspend(void)
{
	u32 temp;
	u64 status;
	u64 lvl;
	u64 wake_level;
	u64 wake_enb;

	clear_pmc_sw_wake_status();

	temp = readl(pmc + PMC_CTRL);
	temp |= PMC_CTRL_LATCH_WAKEUPS;
	pmc_32kwritel(temp, PMC_CTRL);

	temp &= ~PMC_CTRL_LATCH_WAKEUPS;
	pmc_32kwritel(temp, PMC_CTRL);

	status = read_pmc_sw_wake_status();

	lvl = read_pmc_wake_level();

	/* flip the wakeup trigger for any-edge triggered pads
	 * which are currently asserting as wakeups */
	lvl ^= status;

	lvl &= tegra_lp0_wake_level_any;

	wake_level = lvl | tegra_lp0_wake_level;
	wake_enb = tegra_lp0_wake_enb;

	if (debug_lp0) {
		wake_level = lvl ^ status;
		wake_enb = 0xffffffff;
	}

	/* Clear PMC Wake Status register while going to suspend */
	temp = readl(pmc + PMC_WAKE_STATUS);
	if (temp)
		pmc_32kwritel(temp, PMC_WAKE_STATUS);

	write_pmc_wake_level(wake_level);

	write_pmc_wake_mask(wake_enb);

	return 0;
}

static struct syscore_ops tegra_pm_irq_syscore_ops = {
	.suspend = tegra_pm_irq_syscore_suspend,
	.resume = tegra_pm_irq_syscore_resume,
};

static int tegra_pm_irq_syscore_init(void)
{
	register_syscore_ops(&tegra_pm_irq_syscore_ops);

	return 0;
}
subsys_initcall(tegra_pm_irq_syscore_init);

#ifdef CONFIG_DEBUG_FS
static int tegra_pm_irq_debug_show(struct seq_file *s, void *data)
{
	int wake;
	int irq;
	struct irq_desc *desc;
	const char *irq_name;

	seq_printf(s, "wake  irq  count  name\n");
	seq_printf(s, "----------------------\n");
	for (wake = 0; wake < PMC_MAX_WAKE_COUNT; wake++) {
		irq = tegra_wake_to_irq(wake);
		if (irq < 0)
			continue;

		desc = irq_to_desc(irq);
		if (tegra_wake_irq_count[wake] == 0 && desc->action == NULL)
			continue;

		irq_name = (desc->action && desc->action->name) ?
			desc->action->name : "???";

		seq_printf(s, "%4d  %3d  %5d  %s\n",
			wake, irq, tegra_wake_irq_count[wake], irq_name);
	}
	return 0;
}

static int tegra_pm_irq_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, tegra_pm_irq_debug_show, NULL);
}

static const struct file_operations tegra_pm_irq_debug_fops = {
	.open		= tegra_pm_irq_debug_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init tegra_pm_irq_debug_init(void)
{
	struct dentry *d;

	d = debugfs_create_file("wake_irq", S_IRUGO, NULL, NULL,
		&tegra_pm_irq_debug_fops);
	if (!d) {
		pr_err("Failed to create suspend_mode debug file\n");
		return -ENOMEM;
	}

	return 0;
}

late_initcall(tegra_pm_irq_debug_init);
#endif
