// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2023 Andes Technology Corporation
 * Rick Chen, Andes Technology Corporation <rick@andestech.com>
 */

/* CPU specific code */
#include <cpu_func.h>
#include <irq_func.h>
#include <asm/cache.h>
#include <asm/csr.h>
#include <asm/arch-andes/csr.h>

/*
 * cleanup_before_linux() is called just before we call linux
 * it prepares the processor for linux
 *
 * we disable interrupt and caches.
 */
int cleanup_before_linux(void)
{
	disable_interrupts();

	cache_flush();

	return 0;
}

void harts_early_init(void)
{
	/* Enable I/D-cache in SPL */
	if (CONFIG_IS_ENABLED(RISCV_MMODE)) {
		unsigned long mcache_ctl_val = csr_read(CSR_MCACHE_CTL);
		unsigned long mmisc_ctl_val = csr_read(CSR_MMISC_CTL);

		/*
		 * Gowin AE350 (Andes A25): cache ECC (IC/DC_ECCEN) BREAKS DDR AMO
		 * on this gateware (the cache SRAM has no ECC) - amoswap then hangs.
		 * The SDK BSP runs with mcache_ctl=0x3 (only I/D enable). Clear ECC,
		 * prefetch and write-around so atomics work on DDR.
		 *
		 * Keep CCTL_SUEN SET (not cleared): it enables S/U-mode access to the
		 * AndeStar V5 CCTL cache-control CSRs (ucctlbeginaddr 0x80b /
		 * ucctlcommand 0x80c) so the S-mode kernel can do explicit L1 cache
		 * maintenance (e.g. non-coherent DMA wb/inval). It is unrelated to the
		 * AMO hang (that is DC_COHEN/no-coherence-manager); without it an S-mode
		 * csrw 0x80b traps as an illegal instruction.
		 */
		mcache_ctl_val &= ~(MCACHE_CTL_IC_PREFETCH_EN | MCACHE_CTL_DC_PREFETCH_EN | \
				MCACHE_CTL_DC_WAROUND_EN | MCACHE_CTL_L2C_WAROUND_EN | \
				MCACHE_CTL_IC_ECCEN | MCACHE_CTL_DC_ECCEN | MCACHE_CTL_TLB_ECCEN);

		/* Expose the CCTL cache-control registers to S/U mode (Linux). */
		mcache_ctl_val |= MCACHE_CTL_CCTL_SUEN;

		if (!CONFIG_IS_ENABLED(SYS_ICACHE_OFF))
			mcache_ctl_val |= MCACHE_CTL_IC_EN;

		if (!CONFIG_IS_ENABLED(SYS_DCACHE_OFF))
			mcache_ctl_val |= (MCACHE_CTL_DC_EN | MCACHE_CTL_DC_COHEN);

		csr_write(CSR_MCACHE_CTL, mcache_ctl_val);

		if (!CONFIG_IS_ENABLED(SYS_DCACHE_OFF)) {
			/*
			 * Check mcache_ctl.DC_COHEN, we assume this platform does
			 * not support CM if the bit is hard-wired to 0.
			 */
			if (csr_read(CSR_MCACHE_CTL) & MCACHE_CTL_DC_COHEN) {
				/* Wait for DC_COHSTA bit to be set */
				while (!(csr_read(CSR_MCACHE_CTL) & MCACHE_CTL_DC_COHSTA));
			}
		}

		mmisc_ctl_val |= MMISC_CTL_NON_BLOCKING_EN;

		csr_write(CSR_MMISC_CTL, mmisc_ctl_val);
	}
}
