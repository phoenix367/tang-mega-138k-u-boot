// SPDX-License-Identifier: GPL-2.0+
/*
 * Gowin AE350 demo (Tang Mega 138K Pro Dock) RGB LCD scan-out core.
 *
 * A fixed-timing RGB565 framebuffer scanner in the FPGA fabric (src/rtl/video
 * in the gateware repository), registers on the SoC's EXTS bus at 0xE8001000.
 * U-Boot uses it for the boot splash: the framebuffer lives at a fixed DRAM
 * address given by the DT ("framebuffer"), below U-Boot's own relocation area
 * and reserved in the Linux device tree so the picture survives until the
 * kernel's gowin-lcdfb driver takes the panel over.
 */
#include <dm.h>
#include <video.h>
#include <cpu_func.h>
#include <asm/io.h>
#include <linux/bitops.h>

#define LCD_CTRL	0x00	/* [0] enable  [1] test pattern */
#define LCD_FB_BASE	0x08	/* 32-byte aligned */
#define LCD_STRIDE	0x0c	/* bytes per line, multiple of 32 */
#define LCD_HTIM	0x10	/* [15:0] active  [31:16] total */
#define LCD_HSTART	0x14
#define LCD_VTIM	0x18
#define LCD_VSTART	0x1c
#define LCD_ID		0x2c
#define LCD_ID_VALUE	0x56494430	/* "VID0" */
#define CTRL_ENABLE	BIT(0)

struct gowin_lcd_priv {
	void __iomem *base;
	u32 width, height, hfp, hbp, vfp, vbp;
};

static int gowin_lcd_of_to_plat(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	struct gowin_lcd_priv *priv = dev_get_priv(dev);

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;
	priv->width = dev_read_u32_default(dev, "width", 800);
	priv->height = dev_read_u32_default(dev, "height", 480);
	priv->hfp = dev_read_u32_default(dev, "hfront-porch", 210);
	priv->hbp = dev_read_u32_default(dev, "hback-porch", 182);
	priv->vfp = dev_read_u32_default(dev, "vfront-porch", 45);
	priv->vbp = dev_read_u32_default(dev, "vback-porch", 8);

	/* fixed framebuffer, not carved from the top of RAM by the uclass */
	plat->base = dev_read_u32_default(dev, "framebuffer", 0);
	plat->size = priv->width * priv->height * 2;
	if (!plat->base || (plat->base & 31) || (priv->width & 15))
		return -EINVAL;
	return 0;
}

static int gowin_lcd_probe(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	struct gowin_lcd_priv *p = dev_get_priv(dev);
	u32 id = readl(p->base + LCD_ID);

	if (id != LCD_ID_VALUE) {
		printf("gowin_lcd: no scan-out core at %p (id %08x)\n", p->base, id);
		return -ENODEV;
	}

	/* start from a black frame, not whatever DRAM holds */
	memset((void *)(uintptr_t)plat->base, 0, plat->size);
	flush_dcache_range(plat->base, plat->base + plat->size);

	writel(0, p->base + LCD_CTRL);
	writel(plat->base, p->base + LCD_FB_BASE);
	writel(p->width * 2, p->base + LCD_STRIDE);
	writel(((p->hbp + p->width + p->hfp) << 16) | p->width, p->base + LCD_HTIM);
	writel(p->hbp, p->base + LCD_HSTART);
	writel(((p->vbp + p->height + p->vfp) << 16) | p->height, p->base + LCD_VTIM);
	writel(p->vbp, p->base + LCD_VSTART);
	writel(CTRL_ENABLE, p->base + LCD_CTRL);

	uc_priv->xsize = p->width;
	uc_priv->ysize = p->height;
	uc_priv->bpix = VIDEO_BPP16;
	uc_priv->format = VIDEO_UNKNOWN;	/* plain RGB565 */
	video_set_flush_dcache(dev, true);	/* the core reads DRAM, not the cache */
	return 0;
}

static const struct udevice_id gowin_lcd_ids[] = {
	{ .compatible = "gowin,ae350-lcd" },
	{ }
};

U_BOOT_DRIVER(gowin_lcd) = {
	.name		= "gowin_lcd",
	.id		= UCLASS_VIDEO,
	.of_match	= gowin_lcd_ids,
	.of_to_plat	= gowin_lcd_of_to_plat,
	.probe		= gowin_lcd_probe,
	.priv_auto	= sizeof(struct gowin_lcd_priv),
};
