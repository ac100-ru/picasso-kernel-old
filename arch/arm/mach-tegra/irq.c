/*
 * Copyright (C) 2011 Google, Inc.
 *
 * Author:
 *	Colin Cross <ccross@android.com>
 *
 * Copyright (C) 2010, NVIDIA Corporation
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
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/syscore_ops.h>

#include <asm/hardware/gic.h>

#include <mach/pm-irq.h>

#include "board.h"
#include "iomap.h"

#define ICTLR_CPU_IEP_VFIQ	0x08
#define ICTLR_CPU_IEP_FIR	0x14
#define ICTLR_CPU_IEP_FIR_SET	0x18
#define ICTLR_CPU_IEP_FIR_CLR	0x1c

#define ICTLR_CPU_IER		0x20
#define ICTLR_CPU_IER_SET	0x24
#define ICTLR_CPU_IER_CLR	0x28
#define ICTLR_CPU_IEP_CLASS	0x2C

#define ICTLR_COP_IER		0x30
#define ICTLR_COP_IER_SET	0x34
#define ICTLR_COP_IER_CLR	0x38
#define ICTLR_COP_IEP_CLASS	0x3c

#define FIRST_LEGACY_IRQ 32

static int num_ictlrs;

static void __iomem *ictlr_reg_base[] = {
	IO_ADDRESS(TEGRA_PRIMARY_ICTLR_BASE),
	IO_ADDRESS(TEGRA_SECONDARY_ICTLR_BASE),
	IO_ADDRESS(TEGRA_TERTIARY_ICTLR_BASE),
	IO_ADDRESS(TEGRA_QUATERNARY_ICTLR_BASE),
	IO_ADDRESS(TEGRA_QUINARY_ICTLR_BASE),
};

static inline void tegra_irq_write_mask(unsigned int irq, unsigned long reg)
{
	void __iomem *base;
	u32 mask;

	BUG_ON(irq < FIRST_LEGACY_IRQ ||
		irq >= FIRST_LEGACY_IRQ + num_ictlrs * 32);

	base = ictlr_reg_base[(irq - FIRST_LEGACY_IRQ) / 32];
	mask = BIT((irq - FIRST_LEGACY_IRQ) % 32);

	__raw_writel(mask, base + reg);
}

static void tegra_mask(struct irq_data *d)
{
	if (d->irq < FIRST_LEGACY_IRQ)
		return;

	tegra_irq_write_mask(d->irq, ICTLR_CPU_IER_CLR);
}

static void tegra_unmask(struct irq_data *d)
{
	if (d->irq < FIRST_LEGACY_IRQ)
		return;

	tegra_irq_write_mask(d->irq, ICTLR_CPU_IER_SET);
}

static void tegra_ack(struct irq_data *d)
{
	if (d->irq < FIRST_LEGACY_IRQ)
		return;

	tegra_irq_write_mask(d->irq, ICTLR_CPU_IEP_FIR_CLR);
}

static void tegra_eoi(struct irq_data *d)
{
	if (d->irq < FIRST_LEGACY_IRQ)
		return;

	tegra_irq_write_mask(d->irq, ICTLR_CPU_IEP_FIR_CLR);
}

static int tegra_retrigger(struct irq_data *d)
{
	if (d->irq < FIRST_LEGACY_IRQ)
		return 0;

	tegra_irq_write_mask(d->irq, ICTLR_CPU_IEP_FIR_SET);

	return 1;
}

static int tegra_set_type(struct irq_data *d, unsigned int flow_type)
{
	return tegra_pm_irq_set_wake_type(d->irq, flow_type);
}

#ifdef CONFIG_PM_SLEEP
static u32 *cop_ier;
static u32 *cop_iep;
static u32 *cpu_ier;
static u32 *cpu_iep;

/*
 * Caller ensures that tegra_set_wake (irq_set_wake callback)
 * is called for non-gpio wake sources only
 */
static int tegra_set_wake(struct irq_data *d, unsigned int enable)
{
	int ret;

	/* pmc lp0 wake enable for non-gpio wake sources */
	ret = tegra_pm_irq_set_wake(d->irq, enable);
	if (ret)
		pr_err("Failed lp0 wake %s for irq=%d\n",
			(enable ? "enable" : "disable"), d->irq);

	return ret;
}

static int tegra_legacy_irq_suspend(void)
{
	unsigned long flags;
	int i;

	local_irq_save(flags);
	for (i = 0; i < num_ictlrs; i++) {
		void __iomem *ictlr = ictlr_reg_base[i];
		/* save interrupt state */
		cpu_ier[i] = readl(ictlr + ICTLR_CPU_IER);
		cpu_iep[i] = readl(ictlr + ICTLR_CPU_IEP_CLASS);
		cop_ier[i] = readl(ictlr + ICTLR_COP_IER);
		cop_iep[i] = readl(ictlr + ICTLR_COP_IEP_CLASS);

		/* disable COP interrupts */
		writel(~0, ictlr + ICTLR_COP_IER_CLR);
	}
	local_irq_restore(flags);

	return 0;
}

static void tegra_legacy_irq_resume(void)
{
	unsigned long flags;
	int i;

	local_irq_save(flags);
	for (i = 0; i < num_ictlrs; i++) {
		void __iomem *ictlr = ictlr_reg_base[i];
		writel(cpu_iep[i], ictlr + ICTLR_CPU_IEP_CLASS);
		writel(~0ul, ictlr + ICTLR_CPU_IER_CLR);
		writel(cpu_ier[i], ictlr + ICTLR_CPU_IER_SET);
		writel(cop_iep[i], ictlr + ICTLR_COP_IEP_CLASS);
		writel(~0ul, ictlr + ICTLR_COP_IER_CLR);
		writel(cop_ier[i], ictlr + ICTLR_COP_IER_SET);
	}
	local_irq_restore(flags);
}

static struct syscore_ops tegra_legacy_irq_syscore_ops = {
	.suspend = tegra_legacy_irq_suspend,
	.resume = tegra_legacy_irq_resume,
};

static int tegra_legacy_irq_syscore_init(void)
{
	register_syscore_ops(&tegra_legacy_irq_syscore_ops);

	cop_ier = kzalloc(sizeof(u32) * num_ictlrs, GFP_KERNEL);
	cop_iep = kzalloc(sizeof(u32) * num_ictlrs, GFP_KERNEL);
	cpu_ier = kzalloc(sizeof(u32) * num_ictlrs, GFP_KERNEL);
	cpu_iep = kzalloc(sizeof(u32) * num_ictlrs, GFP_KERNEL);

	BUG_ON(!cop_ier || !cop_iep || !cpu_ier || !cpu_iep);

	return 0;
}
subsys_initcall(tegra_legacy_irq_syscore_init);
#else
#define tegra_set_wake NULL
#endif

void __init tegra_init_irq(void)
{
	int i;
	void __iomem *distbase;

	distbase = IO_ADDRESS(TEGRA_ARM_INT_DIST_BASE);
	num_ictlrs = readl_relaxed(distbase + GIC_DIST_CTR) & 0x1f;

	if (num_ictlrs > ARRAY_SIZE(ictlr_reg_base)) {
		WARN(1, "Too many (%d) interrupt controllers found. Maximum is %d.",
			num_ictlrs, ARRAY_SIZE(ictlr_reg_base));
		num_ictlrs = ARRAY_SIZE(ictlr_reg_base);
	}

	for (i = 0; i < num_ictlrs; i++) {
		void __iomem *ictlr = ictlr_reg_base[i];
		writel(~0, ictlr + ICTLR_CPU_IER_CLR);
		writel(0, ictlr + ICTLR_CPU_IEP_CLASS);
	}

	gic_arch_extn.irq_ack = tegra_ack;
	gic_arch_extn.irq_eoi = tegra_eoi;
	gic_arch_extn.irq_mask = tegra_mask;
	gic_arch_extn.irq_unmask = tegra_unmask;
	gic_arch_extn.irq_retrigger = tegra_retrigger;
	gic_arch_extn.irq_set_type = tegra_set_type;
	gic_arch_extn.irq_set_wake = tegra_set_wake;
	gic_arch_extn.flags = IRQCHIP_MASK_ON_SUSPEND;

	/*
	 * Check if there is a devicetree present, since the GIC will be
	 * initialized elsewhere under DT.
	 */
	if (!of_have_populated_dt())
		gic_init(0, 29, distbase,
			IO_ADDRESS(TEGRA_ARM_PERIF_BASE + 0x100));
}

void tegra_init_legacy_irq_cop(void)
{
	int i;

	for (i = 0; i < num_ictlrs; i++) {
		void __iomem *ictlr = ictlr_reg_base[i];
		writel(~0, ictlr + ICTLR_COP_IER_CLR);
		writel(0, ictlr + ICTLR_COP_IEP_CLASS);
	}
}
