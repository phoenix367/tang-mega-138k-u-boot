# U-Boot for the Gowin AE350 SoC on the Sipeed Tang Mega 138K Pro Dock

Mainline U-Boot (`master`, v2026.07-rc3 era) with the small set of changes needed to run
on the Andes AE350 RISC-V SoC inside the GW5AST-138B, as built by the
[tang-mega-138K-Pro-demo](https://github.com/phoenix367/tang-mega-138K-Pro-demo) gateware.
Upstream is tracked as the `upstream` remote; everything board-specific is in one commit on
top of it.

## What is changed

- `configs/ae350_rv32_tang_mega_138k_defconfig` — the working configuration: XIP from SPI
  flash, single hart (no SMP), no EFI loader, env nowhere (the SoC has no `atcspi200` SPI
  master, only the XIP flash interface), `bootcmd` = gunzip the OpenSBI+kernel payload from
  flash and `sdboot`.
- `arch/riscv/dts/ae350_32.dts`, `ae350-u-boot.dtsi` — one hart, `rv32imac`, 50 MHz
  machine timer and APB clocks, DDR3 at 0, mainline-only nodes (virtio, CFI flash, SPI
  master, MAC, LCD, sound) removed.
- `arch/riscv/cpu/andes/cpu.c` — `mcache_ctl`: clear the cache ECC / prefetch / write-around
  bits (ECC breaks DDR atomics on this gateware) and keep `CCTL_SUEN` set so the S-mode
  kernel can issue CCTL cache maintenance.
- `board/andestech/ae350/sdread.c` — `sdinit` / `sdread` / `sdboot` commands for the SDK's
  `apb_sd_top` SD controller on the external APB bus, plus an `amotest` AMO probe.
- `include/configs/ae350.h` — `payloadaddr` / `bootpayload` environment defaults.

## Build

The RISC-V link needs a PIE-capable linker, so use the AMD/Xilinx Linux-gnu toolchain,
with the elf toolchain's hard-float libgcc (the AE350 here has an FPU):

```
LGNU=/mnt/data/xilinx/2025.2/gnu/riscv/linux_toolchain/lin32/bin/riscv32-amd-linux-gnu-
make CROSS_COMPILE=$LGNU ae350_rv32_tang_mega_138k_defconfig
make -j CROSS_COMPILE=$LGNU \
     PLATFORM_LIBGCC="-L /usr/lib/gcc/riscv64-unknown-elf/13.2.0/rv32imafdc/ilp32d -lgcc"
```

Flash image: `u-boot.bin` padded to 0xF0000 bytes followed by `ae350_32.dtb` (the board
code looks for the DTB at CPU address 0x800F0000 = flash offset 0x6F0000), written to SPI
flash at 0x600000. See `BOOT.md` in the gateware repository for the full flash layout.
