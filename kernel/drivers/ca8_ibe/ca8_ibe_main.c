// SPDX-License-Identifier: GPL-2.0-only
/*
 * Report, and optionally set, ACTLR.IBE (bit 6) on the device's
 * Cortex-A8.
 *
 * The context-switch BTB flush vanilla 4.14.52 already does on this core
 * (cpu_ca8_switch_mm) is a no-op unless IBE is set, and the kernel's own IBE
 * write (__ca8_errata, ARM_ERRATA_430973) is compiled out under
 * ARCH_MULTIPLATFORM. ACTLR is secure-only for writes, so set_ibe=1 goes
 * through the GP ROM monitor (ca8_ibe_smc.S).
 *
 * The module stays resident and does nothing after init: returning an error
 * to make it one-shot would print an insmod failure on the boot console every
 * boot. To measure again, rmmod and insmod it.
 *
 * Idle does not undo the write: before every MPU low-power entry
 * omap34xx_save_context() (pm34xx.c) saves the live ACTLR, and the OFF-mode
 * restore (sleep34xx.S) writes that saved value back through the same ROM
 * service. RET keeps the MPU logic, so ACTLR is untouched there.
 */
#include <linux/bitops.h>
#include <linux/irqflags.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/preempt.h>
#include <asm/cputype.h>

#define ACTLR_IBE		BIT(6)
/* arch/arm/mach-omap2/soc.h, which an M= build cannot include */
#define ARCA_OMAP2_DEVICE_TYPE_GP	3
int omap_type(void);		/* arch/arm/mach-omap2/id.c, EXPORT_SYMBOL */

asmlinkage void ca8_ibe_rom_write_actlr(u32 val);

static bool set_ibe;
module_param(set_ibe, bool, 0444);
MODULE_PARM_DESC(set_ibe, "if ACTLR.IBE is clear, set it through the GP ROM (default: measure only)");

static inline u32 read_actlr(void)
{
	u32 v;

	asm volatile("mrc p15, 0, %0, c1, c0, 1" : "=r" (v));
	return v;
}

static int __init ca8_ibe_init(void)
{
	unsigned long flags;
	u32 before, after;

	if (read_cpuid_part() != ARM_CPU_PART_CORTEX_A8) {
		pr_info("ca8_ibe: not a Cortex-A8 (MIDR 0x%08x), nothing to do\n",
			read_cpuid_id());
		return -ENODEV;
	}

	before = read_actlr();
	pr_info("ca8_ibe: ACTLR=0x%08x IBE=%u\n", before, !!(before & ACTLR_IBE));
	if (!set_ibe || (before & ACTLR_IBE))
		return 0;
	if (omap_type() != ARCA_OMAP2_DEVICE_TYPE_GP) {
		pr_warn("ca8_ibe: not a GP OMAP (type %d), the ROM service differs; not writing\n",
			omap_type());
		return 0;
	}

	/* One CPU, nothing may run between the read and the write. */
	preempt_disable();
	local_irq_save(flags);
	ca8_ibe_rom_write_actlr(read_actlr() | ACTLR_IBE);
	after = read_actlr();
	local_irq_restore(flags);
	preempt_enable();

	pr_info("ca8_ibe: wrote ACTLR|IBE through the ROM, readback ACTLR=0x%08x IBE=%u\n",
		after, !!(after & ACTLR_IBE));
	return 0;
}

static void __exit ca8_ibe_exit(void)
{
}

module_init(ca8_ibe_init);
module_exit(ca8_ibe_exit);

MODULE_DESCRIPTION("Report and set Cortex-A8 ACTLR.IBE on the device");
MODULE_LICENSE("GPL v2");
