obj-y                                   += common.o
obj-y                                   += io.o
obj-y                                   += irq.o
obj-y                                   += clock.o
obj-y                                   += timer.o
obj-y					+= fuse.o
obj-y					+= pmc.o
obj-y					+= flowctrl.o
obj-y					+= powergate.o
obj-y					+= apbio.o
obj-y					+= pm.o
obj-y                                   += mc.o
obj-y                                   += csi.o
obj-y                                   += kfuse.o
obj-y                                   += wakeups.o
obj-$(CONFIG_PM_SLEEP)                  += pm-irq.o
obj-$(CONFIG_CPU_IDLE)			+= cpuidle.o
obj-$(CONFIG_CPU_IDLE)			+= sleep.o
obj-$(CONFIG_ARCH_TEGRA_2x_SOC)         += tegra20_clocks.o
obj-$(CONFIG_ARCH_TEGRA_2x_SOC)         += tegra20_clocks_data.o
obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= tegra20_speedo.o
obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= tegra2_emc.o
obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= sleep-tegra20.o
ifeq ($(CONFIG_CPU_IDLE),y)
obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= cpuidle-tegra20.o
endif
ifeq ($(CONFIG_PM_SLEEP),y)
obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= pm-t2.o
endif
obj-$(CONFIG_ARCH_TEGRA_3x_SOC)		+= tegra30_clocks.o
obj-$(CONFIG_ARCH_TEGRA_3x_SOC)		+= tegra30_clocks_data.o
obj-$(CONFIG_ARCH_TEGRA_3x_SOC)		+= tegra30_speedo.o
obj-$(CONFIG_ARCH_TEGRA_3x_SOC)		+= sleep-tegra30.o
ifeq ($(CONFIG_CPU_IDLE),y)
obj-$(CONFIG_ARCH_TEGRA_3x_SOC)		+= cpuidle-tegra30.o
endif
obj-$(CONFIG_SMP)			+= platsmp.o headsmp.o
obj-$(CONFIG_SMP)                       += reset.o
obj-$(CONFIG_HOTPLUG_CPU)               += hotplug.o
obj-$(CONFIG_CPU_FREQ)                  += cpu-tegra.o
obj-$(CONFIG_TEGRA_PCI)			+= pcie.o

obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= board-dt-tegra20.o
obj-$(CONFIG_ARCH_TEGRA_3x_SOC)		+= board-dt-tegra30.o

obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= board-harmony-pcie.o

obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= board-paz00.o

obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= bluetooth-power.o
obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= board-picasso.o

obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= dvfs.o
