/*
 * arch/arm/mach-tegra/sleep.S
 *
 * Copyright (c) 2010-2011, NVIDIA Corporation.
 * Copyright (c) 2011, Google, Inc.
 *
 * Author: Colin Cross <ccross@android.com>
 *         Gary King <gking@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/linkage.h>

#include <asm/assembler.h>
#include <asm/cache.h>
#include <asm/cp15.h>
#include <asm/hardware/cache-l2x0.h>

#include "iomap.h"

#include "flowctrl.h"
#include "sleep.h"

#ifdef CONFIG_PM_SLEEP
/*
 * tegra_disable_clean_inv_dcache
 *
 * disable, clean & invalidate the D-cache
 *
 * Corrupted registers: r1-r3, r6, r8, r9-r11
 */
ENTRY(tegra_disable_clean_inv_dcache)
	stmfd	sp!, {r0, r4-r5, r7, r9-r11, lr}
	dmb					@ ensure ordering

	/* Disable the D-cache */
	mrc	p15, 0, r2, c1, c0, 0
	bic	r2, r2, #CR_C
	mcr	p15, 0, r2, c1, c0, 0
	isb

	/* Flush the D-cache */
	bl	v7_flush_dcache_louis

	/* Trun off coherency */
	exit_smp r4, r5

	ldmfd	sp!, {r0, r4-r5, r7, r9-r11, pc}
ENDPROC(tegra_disable_clean_inv_dcache)

/*
 * tegra_sleep_cpu_finish(unsigned long v2p)
 *
 * enters suspend in LP2 by turning off the mmu and jumping to
 * tegra?_tear_down_cpu
 */
ENTRY(tegra_sleep_cpu_finish)
	/* Flush and disable the L1 data cache */
	bl	tegra_disable_clean_inv_dcache

	mov32	r6, tegra_tear_down_cpu
	ldr	r1, [r6]
	add	r1, r1, r0

	mov32	r3, tegra_shut_off_mmu
	add	r3, r3, r0
	mov	r0, r1

	mov	pc, r3
ENDPROC(tegra_sleep_cpu_finish)

/*
 * tegra_shut_off_mmu
 *
 * r0 = physical address to jump to with mmu off
 *
 * called with VA=PA mapping
 * turns off MMU, icache, dcache and branch prediction
 */
	.align	L1_CACHE_SHIFT
	.pushsection	.idmap.text, "ax"
ENTRY(tegra_shut_off_mmu)
	mrc	p15, 0, r3, c1, c0, 0
	movw	r2, #CR_I | CR_Z | CR_C | CR_M
	bic	r3, r3, r2
	dsb
	mcr	p15, 0, r3, c1, c0, 0
	isb
#ifdef CONFIG_CACHE_L2X0
	/* Disable L2 cache */
	mov32	r4, TEGRA_ARM_PL310_BASE
	mov	r5, #0
	str	r5, [r4, #L2X0_CTRL]
#endif
	mov	pc, r0
ENDPROC(tegra_shut_off_mmu)
	.popsection
#endif
