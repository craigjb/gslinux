/*
 * Gameslab LCD frame buffer driver
 *
 * Author: Craig Bishop
 *         craig@craigjb.com
 *
 * Author: MontaVista Software, Inc.
 *         source@mvista.com
 *
 * 2017 (c) Craig Bishop
 * 2002-2007 (c) MontaVista Software, Inc.
 * 2007 (c) Secret Lab Technologies, Ltd.
 * 2009 (c) Xilinx Inc.
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2.  This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

/*
 * This driver was based on au1100fb.c by MontaVista rewritten for 2.6
 * by Embedded Alley Solutions <source@embeddedalley.com>, which in turn
 * was based on skeletonfb.c, Skeleton for a frame buffer device by
 * Geert Uytterhoeven.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/dma-mapping.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/slab.h>

#ifdef CONFIG_PPC_DCR
#include <asm/dcr.h>
#endif

#define DRIVER_NAME		"gslcdfb"


/*
 * Register offsets
 */
#define REG_OFF_EN 0
#define REG_OFF_FB_PTR 1

/*
 * The hardware supports 800x480 @ 24bpp. Each pixel is 3 bytes.
 */
#define BYTES_PER_PIXEL	3
#define BITS_PER_PIXEL	(BYTES_PER_PIXEL * 8)

#define RED_SHIFT	16
#define GREEN_SHIFT	8
#define BLUE_SHIFT	0

#define PALETTE_ENTRIES_NO	16	/* passed to fb_alloc_cmap() */

/* ML300/403 reference design framebuffer driver platform data struct */
struct gslcdfb_platform_data {
	u32 screen_height_mm;   /* Physical dimensions of screen in mm */
	u32 screen_width_mm;
	u32 xres, yres;         /* resolution of screen in pixels */
	u32 xvirt, yvirt;       /* resolution of memory buffer */

	/* Physical address of framebuffer memory; If non-zero, driver
	* will use provided memory address instead of allocating one from
	* the consistent pool. */
	u32 fb_phys;
};

/*
 * Default gslcdfb configuration
 */
static struct gslcdfb_platform_data gslcd_fb_default_pdata = {
    .screen_height_mm = 65,
    .screen_width_mm = 108,
	.xres = 800,
	.yres = 480,
	.xvirt = 800,
	.yvirt = 480,
};

/*
 * Here are the default fb_fix_screeninfo and fb_var_screeninfo structures
 */
static struct fb_fix_screeninfo gslcd_fb_fix = {
	.id =		"gslcd",
	.type =		FB_TYPE_PACKED_PIXELS,
	.visual =	FB_VISUAL_TRUECOLOR,
	.accel =	FB_ACCEL_NONE
};

static struct fb_var_screeninfo gslcd_fb_var = {
	.bits_per_pixel =	BITS_PER_PIXEL,

	.red =		{ RED_SHIFT, 8, 0 },
	.green =	{ GREEN_SHIFT, 8, 0 },
	.blue =		{ BLUE_SHIFT, 8, 0 },
	.transp =	{ 0, 0, 0 },

	.activate =	FB_ACTIVATE_NOW
};


struct gslcdfb_drvdata {

	struct fb_info	info;		/* FB driver info record */

	phys_addr_t	regs_phys;	/* phys. address of the control
						registers */
	void __iomem	*regs;		/* virt. address of the control
						registers */

	void		*fb_virt;	/* virt. address of the frame buffer */
	dma_addr_t	fb_phys;	/* phys. address of the frame buffer */
	int		fb_alloced;	/* Flag, was the fb memory alloced? */

	u32		pseudo_palette[PALETTE_ENTRIES_NO];
					/* Fake palette of 16 colors */
};

static void gslcd_fb_out32(struct gslcdfb_drvdata *drvdata, u32 offset,
				u32 val)
{
    iowrite32(val, drvdata->regs + (offset << 2));
}

static u32 gslcd_fb_in32(struct gslcdfb_drvdata *drvdata, u32 offset)
{
    return ioread32(drvdata->regs + (offset << 2));
}

static int
gslcd_fb_setcolreg(unsigned regno, unsigned red, unsigned green, unsigned blue,
	unsigned transp, struct fb_info *fbi)
{
	u32 *palette = fbi->pseudo_palette;

	if (regno >= PALETTE_ENTRIES_NO)
		return -EINVAL;

	if (fbi->var.grayscale) {
		/* Convert color to grayscale.
		 * grayscale = 0.30*R + 0.59*G + 0.11*B */
		red = green = blue =
			(red * 77 + green * 151 + blue * 28 + 127) >> 8;
	}

	/* fbi->fix.visual is always FB_VISUAL_TRUECOLOR */

	/* We only handle 8 bits of each color. */
	red >>= 8;
	green >>= 8;
	blue >>= 8;
	palette[regno] = (red << RED_SHIFT) | (green << GREEN_SHIFT) |
			 (blue << BLUE_SHIFT);

	return 0;
}

#define to_gslcdfb_drvdata(_info) \
	container_of(_info, struct gslcdfb_drvdata, info)

static int
gslcd_fb_blank(int blank_mode, struct fb_info *fbi)
{
	struct gslcdfb_drvdata *drvdata = to_gslcdfb_drvdata(fbi);

	switch (blank_mode) {
	case FB_BLANK_UNBLANK:
		/* turn on panel */
		gslcd_fb_out32(drvdata, REG_OFF_EN, 0x1);
		break;

	case FB_BLANK_NORMAL:
	case FB_BLANK_VSYNC_SUSPEND:
	case FB_BLANK_HSYNC_SUSPEND:
	case FB_BLANK_POWERDOWN:
		/* turn off panel */
		gslcd_fb_out32(drvdata, REG_OFF_EN, 0x0);
	default:
		break;

	}
	return 0; /* success */
}

static struct fb_ops gslcdfb_ops =
{
	.owner			= THIS_MODULE,
	.fb_setcolreg		= gslcd_fb_setcolreg,
	.fb_blank		= gslcd_fb_blank,
	.fb_fillrect		= cfb_fillrect,
	.fb_copyarea		= cfb_copyarea,
	.fb_imageblit		= cfb_imageblit,
};

/* ---------------------------------------------------------------------
 * Bus independent setup/teardown
 */

static int gslcdfb_assign(struct platform_device *pdev,
			   struct gslcdfb_drvdata *drvdata,
			   struct gslcdfb_platform_data *pdata)
{
	int rc;
	struct device *dev = &pdev->dev;
	int fbsize = pdata->xvirt * pdata->yvirt * BYTES_PER_PIXEL;

    struct resource *res;
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    drvdata->regs = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(drvdata->regs))
        return PTR_ERR(drvdata->regs);

    drvdata->regs_phys = res->start;

	/* Allocate the framebuffer memory */
	if (pdata->fb_phys) {
		drvdata->fb_phys = pdata->fb_phys;
		drvdata->fb_virt = ioremap(pdata->fb_phys, fbsize);
	} else {
		drvdata->fb_alloced = 1;
		drvdata->fb_virt = dma_alloc_coherent(dev, PAGE_ALIGN(fbsize),
					&drvdata->fb_phys, GFP_KERNEL);
	}

	if (!drvdata->fb_virt) {
		dev_err(dev, "Could not allocate frame buffer memory\n");
		return -ENOMEM;
	}

	/* Clear (turn to black) the framebuffer */
	memset_io((void __iomem *)drvdata->fb_virt, 0, fbsize);

	/* Tell the hardware where the frame buffer is */
	gslcd_fb_out32(drvdata, REG_OFF_FB_PTR, drvdata->fb_phys);

	/* Turn on the display */
	gslcd_fb_out32(drvdata, REG_OFF_EN, 0x1);

	/* Fill struct fb_info */
	drvdata->info.device = dev;
	drvdata->info.screen_base = (void __iomem *)drvdata->fb_virt;
	drvdata->info.fbops = &gslcdfb_ops;
	drvdata->info.fix = gslcd_fb_fix;
	drvdata->info.fix.smem_start = drvdata->fb_phys;
	drvdata->info.fix.smem_len = fbsize;
	drvdata->info.fix.line_length = pdata->xvirt * BYTES_PER_PIXEL;

	drvdata->info.pseudo_palette = drvdata->pseudo_palette;
	drvdata->info.flags = FBINFO_DEFAULT;
	drvdata->info.var = gslcd_fb_var;
	drvdata->info.var.height = pdata->screen_height_mm;
	drvdata->info.var.width = pdata->screen_width_mm;
	drvdata->info.var.xres = pdata->xres;
	drvdata->info.var.yres = pdata->yres;
	drvdata->info.var.xres_virtual = pdata->xvirt;
	drvdata->info.var.yres_virtual = pdata->yvirt;

	/* Allocate a colour map */
	rc = fb_alloc_cmap(&drvdata->info.cmap, PALETTE_ENTRIES_NO, 0);
	if (rc) {
		dev_err(dev, "Fail to allocate colormap (%d entries)\n",
			PALETTE_ENTRIES_NO);
		goto err_cmap;
	}

	/* Register new frame buffer */
	rc = register_framebuffer(&drvdata->info);
	if (rc) {
		dev_err(dev, "Could not register frame buffer\n");
		goto err_regfb;
	}

    /* Put a banner in the log (for DEBUG) */
    dev_dbg(dev, "regs: phys=%pa, virt=%p\n",
        &drvdata->regs_phys, drvdata->regs);

	/* Put a banner in the log (for DEBUG) */
	dev_dbg(dev, "fb: phys=%llx, virt=%p, size=%x\n",
		(unsigned long long)drvdata->fb_phys, drvdata->fb_virt, fbsize);

	return 0;	/* success */

err_regfb:
	fb_dealloc_cmap(&drvdata->info.cmap);

err_cmap:
	if (drvdata->fb_alloced)
		dma_free_coherent(dev, PAGE_ALIGN(fbsize), drvdata->fb_virt,
			drvdata->fb_phys);
	else
		iounmap(drvdata->fb_virt);

	/* Turn off the display */
	gslcd_fb_out32(drvdata, REG_OFF_EN, 0x0);

	return rc;
}

static int gslcdfb_release(struct device *dev)
{
	struct gslcdfb_drvdata *drvdata = dev_get_drvdata(dev);

#if !defined(CONFIG_FRAMEBUFFER_CONSOLE) && defined(CONFIG_LOGO)
	gslcd_fb_blank(VESA_POWERDOWN, &drvdata->info);
#endif

	unregister_framebuffer(&drvdata->info);

	fb_dealloc_cmap(&drvdata->info.cmap);

	if (drvdata->fb_alloced)
		dma_free_coherent(dev, PAGE_ALIGN(drvdata->info.fix.smem_len),
				  drvdata->fb_virt, drvdata->fb_phys);
	else
		iounmap(drvdata->fb_virt);

	/* Turn off the display */
	gslcd_fb_out32(drvdata, REG_OFF_EN, 0x0);

	return 0;
}

/* ---------------------------------------------------------------------
 * OF bus binding
 */

static int gslcdfb_of_probe(struct platform_device *pdev)
{
	struct gslcdfb_platform_data pdata;
	struct gslcdfb_drvdata *drvdata;

	/* Copy with the default pdata (not a ptr reference!) */
	pdata = gslcd_fb_default_pdata;

	/* Allocate the driver data region */
	drvdata = devm_kzalloc(&pdev->dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

    dev_set_drvdata(&pdev->dev, drvdata);
	return gslcdfb_assign(pdev, drvdata, &pdata);
}

static int gslcdfb_of_remove(struct platform_device *op)
{
	return gslcdfb_release(&op->dev);
}

/* Match table for of_platform binding */
static struct of_device_id gslcdfb_of_match[] = {
	{ .compatible = "gslcd", },
	{},
};
MODULE_DEVICE_TABLE(of, gslcdfb_of_match);

static struct platform_driver gslcdfb_of_driver = {
	.probe = gslcdfb_of_probe,
	.remove = gslcdfb_of_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = gslcdfb_of_match,
	},
};

module_platform_driver(gslcdfb_of_driver);

MODULE_AUTHOR("Craig Bishop <craig@craigjb.com>");
MODULE_DESCRIPTION("Gameslab LCD frame buffer driver");
MODULE_LICENSE("GPL");
