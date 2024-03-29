/*
 * Copyright (c) 2010-2012, NVIDIA Corporation. All rights reserved.
 * Copyright (c) 2011, Google, Inc.
 *
 * Author: Colin Cross <ccross@android.com>
 *         Gary King <gking@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/linkage.h>

#include <asm/assembler.h>
#include <asm/cache.h>
#include <asm/domain.h>
#include <asm/memory.h>
#include <asm/page.h>
#include <asm/ptrace.h>
#include <asm/asm-offsets.h>
#include <asm/glue-cache.h>
#include <asm/glue-proc.h>
#include <asm/cp15.h>
#include <asm/hardware/cache-l2x0.h>

#include "sleep.h"
#include "flowctrl.h"

#define EMC_CFG				0xc
#define EMC_ADR_CFG			0x10
#define EMC_REFRESH			0x70
#define EMC_NOP				0xdc
#define EMC_SELF_REF			0xe0
#define EMC_REQ_CTRL			0x2b0
#define EMC_EMC_STATUS			0x2b4

#define CLK_RESET_CCLK_BURST		0x20
#define CLK_RESET_CCLK_DIVIDER		0x24
#define CLK_RESET_SCLK_BURST		0x28
#define CLK_RESET_SCLK_DIVIDER		0x2c

#define CLK_RESET_PLLC_BASE		0x80
#define CLK_RESET_PLLM_BASE		0x90
#define CLK_RESET_PLLP_BASE		0xa0
#define CLK_RESET_PLLP_OUTA		0xa4
#define CLK_RESET_PLLP_OUTB		0xa8
#define CLK_RESET_PLLP_MISC		0xac
#define CLK_RESET_PLLX_BASE		0xe0
#define CLK_RESET_PLLX_MISC		0xe4

#define PMC_SCRATCH41			0x140

#define TEGRA_PL310_VIRT (TEGRA_ARM_PL310_BASE - IO_CPU_PHYS + IO_CPU_VIRT)

#if defined(CONFIG_HOTPLUG_CPU) || defined(CONFIG_PM_SLEEP)
/*
 * tegra20_hotplug_shutdown(void)
 *
 * puts the current cpu in reset
 * should never return
 */
ENTRY(tegra20_hotplug_shutdown)
	/* Turn off SMP coherency */
	exit_smp r4, r5

	/* Put this CPU down */
	cpu_id	r0
	bl	tegra20_cpu_shutdown
	mov	pc, lr			@ should never get here
ENDPROC(tegra20_hotplug_shutdown)

/*
 * tegra20_cpu_shutdown(int cpu)
 *
 * r0 is cpu to reset
 *
 * puts the specified CPU in wait-for-event mode on the flow controller
 * and puts the CPU in reset
 * can be called on the current cpu or another cpu
 * if called on the current cpu, does not return
 * MUST NOT BE CALLED FOR CPU 0.
 *
 * corrupts r0-r3, r12
 */
ENTRY(tegra20_cpu_shutdown)
	cmp	r0, #0
	moveq	pc, lr			@ must not be called for CPU 0

	cpu_to_halt_reg r1, r0
	ldr	r3, =TEGRA_FLOW_CTRL_VIRT
	mov	r2, #FLOW_CTRL_WAITEVENT | FLOW_CTRL_JTAG_RESUME
	str	r2, [r3, r1]		@ put flow controller in wait event mode
	ldr	r2, [r3, r1]
	isb
	dsb
	movw	r1, 0x1011
	mov	r1, r1, lsl r0
	ldr	r3, =TEGRA_CLK_RESET_VIRT
	str	r1, [r3, #0x340]	@ put slave CPU in reset
	isb
	dsb
	cpu_id	r3
	cmp	r3, r0
	beq	.
	mov	pc, lr
ENDPROC(tegra20_cpu_shutdown)
#endif

#ifdef CONFIG_PM_SLEEP
/*
 * tegra20_sleep_core_finish(unsigned long v2p)
 *
 * enters suspend in LP0 or LP1 by turning off the mmu and jumping to
 * tegra20_tear_down_core in IRAM
 */
ENTRY(tegra20_sleep_core_finish)
	stmfd	sp!, {r0}
	dmb					@ ensure ordering

	/* Disable the D-cache */
	mrc	p15, 0, r2, c1, c0, 0
	bic	r2, r2, #CR_C
	mcr	p15, 0, r2, c1, c0, 0
	isb

	/* Flush the D-cache */
	bl	v7_flush_dcache_louis

	/* Sync l2 */
	dsb
	mov32	r1, TEGRA_PL310_VIRT
	mov	r2, #0
	str	r2, [r1, #L2X0_CACHE_SYNC]

	/* Invalidate the TLBs & BTAC */
	mcr	p15, 0, r2, c8, c3, 0	@ invalidate shared TLBs
	mcr	p15, 0, r2, c7, c1, 6	@ invalidate shared BTAC
	dsb
	isb

	/* Trun off coherency */
	exit_smp r4, r5

	ldmfd	sp!, {r0}

	mov32	r1, tegra20_tear_down_core
	mov32	r2, tegra20_iram_start
	sub	r1, r1, r2
	mov32	r2, TEGRA_IRAM_CODE_AREA
	add	r1, r1, r2

	mov32	r3, tegra_shut_off_mmu
	add	r3, r3, r0
	mov	r0, r1

	mov	pc, r3
ENDPROC(tegra20_sleep_core_finish)

	.arm
/* START OF ROUTINES COPIED TO IRAM */
	.align L1_CACHE_SHIFT
	.globl tegra20_iram_start
tegra20_iram_start:

/*
 * tegra20_lp1_reset
 *
 * reset vector for LP1 restore; copied into IRAM during suspend.
 * brings the system back up to a safe starting point (SDRAM out of
 * self-refresh, PLLC, PLLM and PLLP reenabled, CPU running on PLLP,
 * system clock running on the same PLL that it suspended at), and
 * jumps to tegra_resume expected to be stored in PMC_SCRATCH41
 *
 * NOTE: THIS *MUST* BE RELOCATED TO TEGRA_IRAM_CODE_AREA AND MUST BE FIRST.
 */
ENTRY(tegra20_lp1_reset)
	/*
	 * the CPU and system bus are running at 32KHz and executing from
	 * IRAM when this code is executed; immediately switch to CLKM and
	 * enable PLLP.
	 */
	mov32	r0, TEGRA_CLK_RESET_BASE
	/* secure code handles 32KHz to CLKM/OSC clock switch */
	mov	r1, #(1 << 28)
	str	r1, [r0, #CLK_RESET_SCLK_BURST]
	str	r1, [r0, #CLK_RESET_CCLK_BURST]
	mov	r1, #0
	str	r1, [r0, #CLK_RESET_SCLK_DIVIDER]
	str	r1, [r0, #CLK_RESET_CCLK_DIVIDER]
	ldr	r1, [r0, #CLK_RESET_PLLM_BASE]
	tst	r1, #(1 << 30)
	orreq	r1, r1, #(1 << 30)
	streq	r1, [r0, #CLK_RESET_PLLM_BASE]
	ldr	r1, [r0, #CLK_RESET_PLLP_BASE]
	tst	r1, #(1 << 30)
	orreq	r1, r1, #(1 << 30)
	streq	r1, [r0, #CLK_RESET_PLLP_BASE]
	ldr	r1, [r0, #CLK_RESET_PLLC_BASE]
	tst	r1, #(1 << 30)
	orreq	r1, r1, #(1 << 30)
	streq	r1, [r0, #CLK_RESET_PLLC_BASE]

	adr	r2, tegra20_sdram_pad_address
	adr	r4, tegra20_sdram_pad_save
	mov	r5, #0

padload:
	ldr	r0, [r2, r5]		@ r0 is emc register address

	ldr	r1, [r4, r5]
	str	r1, [r0]		@ set emc register to safe vals

	add	r5, r5, #4
	ldr	r0, tegra20_sdram_pad_size
	cmp	r0, r5
	bne	padload

padload_done:
	mov32	r7, TEGRA_TMRUS_BASE
	ldr	r1, [r7]
	add	r1, r1, #0xff		@ 255uS delay for PLL stabilization

1:	ldr	r0, [r7]
	cmp	r0, r1
	dmb
	bmi	1b

	adr	r4, tegra20_sclk_save
	ldr	r4, [r4]
	mov32	r0, TEGRA_CLK_RESET_BASE
	str	r4, [r0, #CLK_RESET_SCLK_BURST]
	ldr	r4, =((1 << 28) | (4))	@ burst policy is PLLP
	str	r4, [r0, #CLK_RESET_CCLK_BURST]

	mov32	r0, TEGRA_EMC_BASE
	ldr	r1, [r0, #EMC_CFG]
	bic	r1, r1, #(1 << 31)	@ disable DRAM_CLK_STOP
	str	r1, [r0, #EMC_CFG]

	mov	r1, #0
	str	r1, [r0, #EMC_SELF_REF]	@ take DRAM out of self refresh
	mov	r1, #1
	str	r1, [r0, #EMC_NOP]
	str	r1, [r0, #EMC_NOP]
	str	r1, [r0, #EMC_REFRESH]

	ldr	r1, [r0, #EMC_ADR_CFG]
	tst	r1, #(0x3 << 24)
	moveq	r1, #(0x1 << 8)		@ just 1 device
	movne	r1, #(0x3 << 8)		@ 2 devices

exit_selfrefresh_loop:
	ldr	r2, [r0, #EMC_EMC_STATUS]
	ands	r2, r2, r1
	bne	exit_selfrefresh_loop

	mov	r1, #0
	str	r1, [r0, #EMC_REQ_CTRL]

	mov32	r0, TEGRA_PMC_BASE
	ldr	r0, [r0, #PMC_SCRATCH41]
	bx	r0
ENDPROC(tegra20_lp1_reset)

/*
 * tegra20_tear_down_core
 *
 * copied into and executed from IRAM
 * puts memory in self-refresh for LP0 and LP1
 */
tegra20_tear_down_core:
 THUMB(	.thumb		)
 THUMB(	.align		)
 THUMB(	bx	pc	)
 THUMB(	nop		)
	.arm
	bl	tegra20_sdram_self_refresh
	bl	tegra20_cpu_clk32k
	b	tegra20_enter_sleep

/*
 * tegra20_cpu_clk32k
 *
 * In LP0 and LP1 all plls will be turned off.  Switch the CPU and system clock
 * to the 32khz clock (clks)
 */
tegra20_cpu_clk32k:
	/* start by jumping to clkm to safely disable PLLs, then jump
	 * to clks */
	mov	r0, #(1 << 28)
	str	r0, [r5, #CLK_RESET_SCLK_BURST]
	str	r0, [r5, #CLK_RESET_CCLK_BURST]
	mov	r0, #0
	str	r0, [r5, #CLK_RESET_CCLK_DIVIDER]
	str	r0, [r5, #CLK_RESET_SCLK_DIVIDER]

	/* 2 us delay between changing sclk and disabling PLLs */
	mov32	r7, TEGRA_TMRUS_BASE
	ldr	r1, [r7]
	add	r1, r1, #3

1:	ldr	r0, [r7]
	cmp	r0, r1
	dmb
	bmi	1b

	/* switch to CLKS */
	mov	r0, #0	/* burst policy = 32KHz */
	str	r0, [r5, #CLK_RESET_SCLK_BURST]

	/* disable PLLP, PLLM, PLLC in LP0 and LP1 states */
	ldr	r0, [r5, #CLK_RESET_PLLM_BASE]
	bic	r0, r0, #(1 << 30)
	str	r0, [r5, #CLK_RESET_PLLM_BASE]
	ldr	r0, [r5, #CLK_RESET_PLLP_BASE]
	bic	r0, r0, #(1 << 30)
	str	r0, [r5, #CLK_RESET_PLLP_BASE]
	ldr	r0, [r5, #CLK_RESET_PLLC_BASE]
	bic	r0, r0, #(1 << 30)
	str	r0, [r5, #CLK_RESET_PLLC_BASE]
	mov	pc, lr

/*
 * tegra20_enter_sleep
 *
 * uses flow controller to enter sleep state
 * executes from IRAM with SDRAM in selfrefresh when target state is LP0 and LP1
 * executes from SDRAM with target state is LP2
 */
tegra20_enter_sleep:
	mov32	r6, TEGRA_FLOW_CTRL_BASE

	mov	r0, #FLOW_CTRL_WAIT_FOR_INTERRUPT
	orr	r0, r0, #FLOW_CTRL_HALT_CPU_IRQ | FLOW_CTRL_HALT_CPU_FIQ
	cpu_id	r1
	cpu_to_halt_reg r1, r1
	str	r0, [r6, r1]
	dsb
	ldr	r0, [r6, r1] /* memory barrier */

halted: dsb
	wfe	/* CPU should be power gated here */
	isb
	b	halted

/*
 * tegra20_sdram_self_refresh
 *
 * called with MMU off and caches disabled
 * puts sdram in self refresh
 * must execute from IRAM
 */
tegra20_sdram_self_refresh:
	mov32	r1, TEGRA_EMC_BASE
	mov	r2, #3
	str	r2, [r1, #EMC_REQ_CTRL]		@ stall incoming DRAM requests

emcidle:ldr	r2, [r1, #EMC_EMC_STATUS]
	tst	r2, #4
	beq	emcidle

	mov	r2, #1
	str	r2, [r1, #EMC_SELF_REF]

	ldr	r2, [r1, #EMC_ADR_CFG]
	tst	r2, #(0x3 << 24)
	moveq	r2, #(0x1 << 8)			@ just 1 device
	movne	r2, #(0x3 << 8)			@ 2 devices

emcself:ldr	r3, [r1, #EMC_EMC_STATUS]
	and	r3, r3, r2
	cmp	r3, r2
	bne	emcself				@ loop until DDR in self-refresh

	adr	r2, tegra20_sdram_pad_address
	adr	r3, tegra20_sdram_pad_safe
	adr	r4, tegra20_sdram_pad_save
	mov	r5, #0

padsave:
	ldr	r0, [r2, r5]			@ r0 is emc register address

	ldr	r1, [r0]
	str	r1, [r4, r5]			@ save emc register

	ldr	r1, [r3, r5]
	str	r1, [r0]			@ set emc register to safe vals

	add	r5, r5, #4
	ldr	r0, tegra20_sdram_pad_size
	cmp	r0, r5
	bne	padsave
padsave_done:

	mov32	r5, TEGRA_CLK_RESET_BASE
	ldr	r0, [r5, #CLK_RESET_SCLK_BURST]
	adr	r2, tegra20_sclk_save
	str	r0, [r2]
	dsb
	mov	pc, lr

tegra20_sdram_pad_address:
	.word	TEGRA_APB_MISC_BASE + 0x8c8 /* XM2CFGCPADCTRL */
	.word	TEGRA_APB_MISC_BASE + 0x8cc /* XM2CFGDPADCTRL */
	.word	TEGRA_APB_MISC_BASE + 0x8d0 /* XM2CLKCFGPADCTRL */
	.word	TEGRA_APB_MISC_BASE + 0x8d4 /* XM2COMPPADCTRL */
	.word	TEGRA_APB_MISC_BASE + 0x8d8 /* XM2VTTGENPADCTRL */
	.word	TEGRA_APB_MISC_BASE + 0x8e4 /* XM2CFGCPADCTRL2 */
	.word	TEGRA_APB_MISC_BASE + 0x8e8 /* XM2CFGDPADCTRL2 */

tegra20_sdram_pad_size:
	.word	tegra20_sdram_pad_size - tegra20_sdram_pad_address

tegra20_sdram_pad_safe:
	.word	0x8
	.word	0x8
	.word	0x0
	.word	0x8
	.word	0x5500
	.word	0x08080040
	.word	0x0

tegra20_sclk_save:
	.word	0x0

tegra20_sdram_pad_save:
	.word	0
	.word	0
	.word	0
	.word	0
	.word	0
	.word	0
	.word	0

	.ltorg
/* dummy symbol for end of IRAM */
	.align L1_CACHE_SHIFT
	.globl tegra20_iram_end
tegra20_iram_end:
	b	.
#endif
