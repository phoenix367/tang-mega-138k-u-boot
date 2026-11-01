// SPDX-License-Identifier: GPL-2.0+
/*
 * Raw-block reader for the SDK's apb_sd_top hardware SD-SPI controller on the
 * Gowin AE350's external APB bus (base 0xF8000000). The controller runs the SD
 * protocol in hardware; software just selects a transaction (INIT / READ),
 * gives a 32-bit block address, triggers, polls the BUSY status, and drains
 * 512 bytes from the RX FIFO. Register map decoded from the SDK bare-metal
 * sd.c/sd.h (word-indexed regs -> byte offset = index*4).
 *
 * Usage at the U-Boot prompt:
 *   sdinit
 *   sdread <dram_addr> <start_block> <count>
 */
#include <command.h>
#include <vsprintf.h>
#include <cpu_func.h>
#include <u-boot/schedule.h>
#include <linux/io.h>
#include <linux/delay.h>

#define SD_BASE			0xF8000000UL
#define SD_R(off)		((void __iomem *)(SD_BASE + (off)))

#define SD_TRANS_TYPE		0x08
#define SD_TRANS_CTRL		0x0C
#define SD_TRANS_STS		0x10
#define SD_TRANS_ERROR		0x14
#define SD_ADDR_7_0		0x1C
#define SD_ADDR_15_8		0x20
#define SD_ADDR_23_16		0x24
#define SD_ADDR_31_24		0x28
#define SD_RX_FIFO_DATA		0x40
#define SD_RX_FIFO_CTRL		0x50

#define TRANS_TYPE_INIT		1
#define TRANS_TYPE_READ		2
#define TRANS_START		1
#define TRANS_BUSY		1
#define ERR_INIT_MASK		0x3
#define ERR_READ_MASK		0xC

#define SD_BLOCK_SIZE		512
#define SD_POLL_TIMEOUT		2000000

static int sd_wait_idle(void)
{
	int t = SD_POLL_TIMEOUT;

	while ((readl(SD_R(SD_TRANS_STS)) & TRANS_BUSY) && --t)
		;
	return t ? 0 : -1;
}

static int sd_init(void)
{
	writel(0, SD_R(SD_TRANS_TYPE));
	writel(0, SD_R(SD_TRANS_CTRL));
	writel(TRANS_TYPE_INIT, SD_R(SD_TRANS_TYPE));
	writel(TRANS_START, SD_R(SD_TRANS_CTRL));

	if (sd_wait_idle()) {
		printf("sd: init busy timeout\n");
		return -1;
	}
	if (readl(SD_R(SD_TRANS_ERROR)) & ERR_INIT_MASK) {
		printf("sd: init error 0x%x\n", readl(SD_R(SD_TRANS_ERROR)));
		return -1;
	}
	return 0;
}

static int sd_read_block(u32 blk, u8 *buf)
{
	int i;

	writel(1, SD_R(SD_RX_FIFO_CTRL));		/* reset RX FIFO */
	writel(blk & 0xff,         SD_R(SD_ADDR_7_0));
	writel((blk >> 8) & 0xff,  SD_R(SD_ADDR_15_8));
	writel((blk >> 16) & 0xff, SD_R(SD_ADDR_23_16));
	writel((blk >> 24) & 0xff, SD_R(SD_ADDR_31_24));
	writel(0, SD_R(SD_TRANS_TYPE));
	writel(TRANS_TYPE_READ, SD_R(SD_TRANS_TYPE));
	writel(TRANS_START, SD_R(SD_TRANS_CTRL));

	if (sd_wait_idle())
		return -1;
	if (readl(SD_R(SD_TRANS_ERROR)) & ERR_READ_MASK)
		return -1;

	(void)readl(SD_R(SD_RX_FIFO_DATA));	/* priming read (per reference sd.c) */
	for (i = 0; i < SD_BLOCK_SIZE; i++)
		buf[i] = readl(SD_R(SD_RX_FIFO_DATA)) & 0xff;

	writel(1, SD_R(SD_RX_FIFO_CTRL));
	return 0;
}

static int do_amotest(struct cmd_tbl *cmdtp, int flag, int argc,
		      char *const argv[])
{
	ulong addr;
	u32 old = 0, rb;

	ulong mcc, mmisc;

	if (argc != 2)
		return CMD_RET_USAGE;
	addr = hextoul(argv[1], NULL);

	__asm__ volatile("csrr %0, 0x7ca" : "=r"(mcc));		/* mcache_ctl */
	__asm__ volatile("csrr %0, 0x7d0" : "=r"(mmisc));	/* mmisc_ctl  */
	printf("amotest @0x%lx: mcache_ctl=0x%lx mmisc_ctl=0x%lx L2ctl=0x%x\n",
	       addr, mcc, mmisc, readl((void *)0xe0500000));
	/* Match bare-metal firmware: clear mmisc_ctl.NON_BLOCKING_EN (bit 8) */
	__asm__ volatile("csrc 0x7d0, %0" :: "r"(0x100UL));
	__asm__ volatile("csrr %0, 0x7d0" : "=r"(mmisc));
	printf("  after clear NON_BLOCKING -> mmisc_ctl=0x%lx\n", mmisc);

	writel(0xdeadbeef, (void *)addr);
	printf("amotest @0x%lx: pre=0x%x  amoswap.w...\n",
	       addr, readl((void *)addr));
	__asm__ volatile("amoswap.w %0, %2, (%1)"
			 : "=r"(old) : "r"(addr), "r"(0x12345678) : "memory");
	rb = readl((void *)addr);
	printf("amotest @0x%lx: OK old=0x%x readback=0x%x\n", addr, old, rb);
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(amotest, 2, 0, do_amotest,
	   "test amoswap.w at a DRAM address (hangs if AMO unsupported there)",
	   "<addr>");

static int do_sdinit(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	if (sd_init())
		return CMD_RET_FAILURE;
	printf("sd: card initialized\n");
	return CMD_RET_SUCCESS;
}

static int do_sdread(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	ulong addr, blk, cnt, i;

	if (argc != 4)
		return CMD_RET_USAGE;

	addr = hextoul(argv[1], NULL);
	blk  = hextoul(argv[2], NULL);
	cnt  = hextoul(argv[3], NULL);

	for (i = 0; i < cnt; i++) {
		if (sd_read_block(blk + i, (u8 *)(addr + i * SD_BLOCK_SIZE))) {
			printf("\nsd: read failed at block %lu\n", blk + i);
			return CMD_RET_FAILURE;
		}
		if ((i & 0xfff) == 0xfff) {
			printf("  %lu/%lu blocks\r", i + 1, cnt);
			schedule();
		}
	}
	printf("sd: read %lu blocks (%lu bytes) to 0x%lx\n",
	       cnt, cnt * SD_BLOCK_SIZE, addr);
	return CMD_RET_SUCCESS;
}

static int do_sdboot(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	void (*fw)(unsigned long hartid, unsigned long fdt);
	ulong addr = 0;

	if (argc >= 2)
		addr = hextoul(argv[1], NULL);
	fw = (void (*)(unsigned long, unsigned long))addr;

	printf("## Booting firmware at 0x%lx  (a0=hartid 0, a1=0, embedded FDT)\n",
	       addr);
	flush_dcache_all();		/* push the loaded image out to DRAM */
	invalidate_icache_all();	/* fence.i: sync I-cache to the new code */

	fw(0, 0);			/* a0 = boot hart 0, a1 = 0 -> OpenSBI uses its built-in FDT */
	return CMD_RET_FAILURE;		/* not reached */
}

U_BOOT_CMD(sdinit, 1, 0, do_sdinit,
	   "initialize the SD card (apb_sd_top @0xF8000000)", "");

U_BOOT_CMD(sdboot, 2, 0, do_sdboot,
	   "jump to a loaded M-mode firmware image with RISC-V boot regs",
	   "[addr]\n"
	   "    - flush caches and enter <addr> with a0=0 (hart), a1=0 (default addr 0)");

U_BOOT_CMD(sdread, 4, 0, do_sdread,
	   "raw SD block read via apb_sd_top",
	   "<dram_addr> <start_block> <count>\n"
	   "    - read <count> 512-byte blocks from the card to DRAM (hex args)");
