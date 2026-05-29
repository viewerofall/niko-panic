// SPDX-License-Identifier: GPL-2.0
// niko-panic — custom kernel panic screen (OneShot / World Machine theme)
//
// Captures the live DRM scanout buffer via a kretprobe on the GPU driver's
// get_scanout_buffer (auto-detected across amdgpu/i915/nouveau/sysfb/...), then
// overdraws drm_panic's screen from a kmsg_dumper at KMSG_DUMP_PANIC time.
// See README.md for the full design.

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kprobes.h>
#include <linux/font.h>
#include <linux/kmsg_dump.h>
#include <linux/string.h>
#include <linux/minmax.h>
#include <linux/iosys-map.h>
#include <linux/fb.h>
#include <linux/panic.h>
#include <drm/drm_panic.h>
#include <drm/drm_plane.h>
#include <drm/drm_device.h>
#include <drm/drm_fb_helper.h>
#include <asm/io.h>

#include "niko_image.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("abyss");
MODULE_DESCRIPTION("OneShot / World Machine kernel panic screen");

// Diagnostic toggles. Set at modprobe time, e.g.:
//   modprobe niko_panic use_skip_panic=0 use_panic_blink=0
// to fall back to the known-good dumper-only behaviour.
static bool use_skip_panic = true;
module_param(use_skip_panic, bool, 0644);
MODULE_PARM_DESC(use_skip_panic, "Set fb skip_panic to suppress fbcon dmesg (default 1)");

// Off by default: set_pixel() on discrete VRAM is slow (per-pixel indirect
// MMIO), so repainting every blink tick would never settle. skip_panic keeps
// the single draw on screen. Enable only on CPU-mapped (APU) setups.
static bool use_panic_blink;
module_param(use_panic_blink, bool, 0644);
MODULE_PARM_DESC(use_panic_blink, "Redraw from panic_blink loop for persistence (default 0)");

// Palette — TWM theme
#define COLOR_BG     0x000a0010u  // #0a0010
#define COLOR_PURPLE 0x00c792eau  // #c792ea
#define COLOR_CYAN   0x0000e5c8u  // #00e5c8
#define COLOR_WHITE  0x00ffffffu
// Sentinel bg meaning "don't paint unlit pixels" — the screen bg is already
// filled, so glyphs only write their lit pixels (far fewer set_pixel calls).
#define COLOR_NONE   0xff000000u

// Layout (tuned for 1280×720)
#define LEFT_W       500
#define DIVIDER_X    510
#define RIGHT_X      524
#define MARGIN_Y     40

// Scanout buffer captured from drm_panic's amdgpu call
static struct drm_scanout_buffer captured_sb;
static bool sb_valid;

// Panic description, saved by the dumper for the panic_blink redraw path.
#define DESC_MAX 96
static char saved_desc[DESC_MAX] = "see log";

// kmsg ring buffer — gathered once on first render, reused on blink redraws.
// Drawing stops when it runs out of vertical space, so a generous count just
// lets the log fill whatever room is left below the panic line.
#define KMSG_LINES   48
#define KMSG_LINE_W  94
static char kbuf[KMSG_LINES][KMSG_LINE_W + 1];
static int  kmsg_count, kmsg_start;
static bool kmsg_gathered;

// ---------------------------------------------------------------------------
// Driver table
//
// Every DRM driver implements the panic scanout hook with the SAME signature:
//   int get_scanout_buffer(struct drm_plane *plane, struct drm_scanout_buffer *sb)
// so one capture handler works for all of them — we just kretprobe by symbol
// name. Each system's GPU driver exports a different symbol; we register a probe
// for every known one and the ones whose driver is loaded arm successfully.
// Detection happens HERE, at module load (or when the GPU driver loads), never
// during a panic.
//
// The set_pixel-vs-map distinction is per-buffer, not per-driver: discrete GPUs
// (VRAM, no CPU access) hand us a set_pixel() MMIO callback; integrated parts
// hand us a CPU-mapped buffer. PUT_PIXEL handles both, so each entry below
// covers its dGPU and iGPU/APU variants automatically.

struct niko_driver {
	const char *symbol;   // kretprobe target
	const char *label;    // human-readable system description
	struct kretprobe probe;
	bool armed;
};

static struct niko_driver niko_drivers[] = {
	// *** abyss's machine: AMD Radeon RX 6700 XT — discrete RDNA2, VRAM, ***
	// *** takes the set_pixel() MMIO path. Same symbol also serves AMD    ***
	// *** APUs (Ryzen iGPU), which take the CPU-mapped path instead.      ***
	{ "amdgpu_display_get_scanout_buffer",
	  "AMD amdgpu — Radeon/RDNA dGPU + Ryzen APU" },

	// Intel integrated graphics (HD/UHD/Iris/Xe) and Arc discrete — i915.
	{ "intel_get_scanout_buffer",
	  "Intel i915 — iGPU + Arc dGPU" },

	// Open-source NVIDIA (Maxwell GM10x and newer) via nouveau.
	{ "nv50_wndw_get_scanout_buffer",
	  "Nouveau — open NVIDIA, GM10x+" },

	// Generic firmware framebuffer: VESA, EFI-GOP, simpledrm. Covers any
	// machine with no native KMS driver (incl. NVIDIA proprietary fallback).
	{ "drm_sysfb_plane_helper_get_scanout_buffer",
	  "Generic VESA/EFI/simpledrm firmware framebuffer" },

	// ARM SoC DMA-backed displays: i.MX (ipuv3), R-Car, Sharp Mobile, TI.
	{ "drm_fb_dma_get_scanout_buffer",
	  "ARM SoC DMA framebuffer — i.MX/R-Car/TI" },

	// Server baseboard management controllers (headless rack boxes).
	{ "ast_primary_plane_helper_get_scanout_buffer",
	  "ASPEED AST BMC — servers" },
	{ "mgag200_primary_plane_helper_get_scanout_buffer",
	  "Matrox MGA G200 BMC — servers" },

	// Virtual machines.
	{ "virtio_drm_get_scanout_buffer",
	  "virtio-gpu — QEMU/KVM VM" },
	{ "bochs_primary_plane_helper_get_scanout_buffer",
	  "bochs / stdvga — QEMU VM" },
	{ "hyperv_plane_get_scanout_buffer",
	  "Hyper-V synthetic video — VM" },
};

#define NIKO_NDRIVERS ARRAY_SIZE(niko_drivers)

struct scanout_capture_data {
	struct drm_plane *plane;
	struct drm_scanout_buffer *sb;
};

static int scanout_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct scanout_capture_data *d = (struct scanout_capture_data *)ri->data;
	// SysV x86_64: arg1 = RDI (plane), arg2 = RSI (scanout buffer)
	d->plane = (struct drm_plane *)regs->di;
	d->sb    = (struct drm_scanout_buffer *)regs->si;
	return 0;
}

static int scanout_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct scanout_capture_data *d = (struct scanout_capture_data *)ri->data;

	// regs->ax is the int return value; 0 = success
	if ((int)(long)regs->ax != 0 || !d->sb)
		return 0;

	memcpy(&captured_sb, d->sb, sizeof(captured_sb));
	sb_valid = true;

	// Suppress fbcon's dmesg takeover: plane->dev->fb_helper->info->skip_panic.
	// Driver-agnostic — every DRM device with fbdev emulation has this path.
	if (use_skip_panic &&
	    d->plane && d->plane->dev && d->plane->dev->fb_helper &&
	    d->plane->dev->fb_helper->info)
		d->plane->dev->fb_helper->info->skip_panic = true;

	return 0;
}

// ---------------------------------------------------------------------------
// Pixel / rect helpers

static inline void put_pixel_iomem(void __iomem *base, u32 pitch,
                                    int x, int y, u32 color,
                                    int xres, int yres)
{
	if ((unsigned)x >= (unsigned)xres || (unsigned)y >= (unsigned)yres)
		return;
	writel(color, (char __iomem *)base + (u32)y * pitch + (u32)x * 4);
}

static inline void put_pixel_mem(void *base, u32 pitch,
                                  int x, int y, u32 color,
                                  int xres, int yres)
{
	if ((unsigned)x >= (unsigned)xres || (unsigned)y >= (unsigned)yres)
		return;
	*((u32 *)((char *)base + (u32)y * pitch + (u32)x * 4)) = color;
}

// Unified helper. amdgpu has two paths: a CPU-mapped buffer (map[0]) OR, for a
// VRAM-only scanout buffer with no CPU access (the discrete-GPU case), a
// set_pixel() MMIO callback with map[0] left null. Prefer set_pixel when set,
// exactly like drm_panic's own draw path.
#define PUT_PIXEL(sb, pitch, x, y, color, w, h) \
	do { \
		if ((sb)->set_pixel) { \
			if ((unsigned)(x) < (unsigned)(w) && (unsigned)(y) < (unsigned)(h)) \
				(sb)->set_pixel((sb), (x), (y), (color)); \
		} else if ((sb)->map[0].is_iomem) { \
			put_pixel_iomem((sb)->map[0].vaddr_iomem, pitch, x, y, color, w, h); \
		} else { \
			put_pixel_mem((sb)->map[0].vaddr, pitch, x, y, color, w, h); \
		} \
	} while (0)

static void fill_rect(struct drm_scanout_buffer *sb, u32 pitch,
                      int x, int y, int w, int h, u32 color,
                      int xres, int yres)
{
	int x1 = max(x, 0), y1 = max(y, 0);
	int x2 = min(x + w, xres), y2 = min(y + h, yres);
	int i, j;

	for (i = y1; i < y2; i++)
		for (j = x1; j < x2; j++)
			PUT_PIXEL(sb, pitch, j, i, color, xres, yres);
}

// ---------------------------------------------------------------------------
// Niko image

static void draw_niko(struct drm_scanout_buffer *sb, u32 pitch,
                      int sx, int sy, int xres, int yres)
{
	int x, y;

	for (y = 0; y < NIKO_H; y++) {
		for (x = 0; x < NIKO_W; x++) {
			int idx = (y * NIKO_W + x) * 3;
			u32 color = ((u32)niko_pixels[idx]     << 16)
			          | ((u32)niko_pixels[idx + 1] << 8)
			          |  (u32)niko_pixels[idx + 2];
			PUT_PIXEL(sb, pitch, sx + x, sy + y, color, xres, yres);
		}
	}
}

// ---------------------------------------------------------------------------
// Font rendering

static void draw_char(struct drm_scanout_buffer *sb, u32 pitch,
                      int x, int y, unsigned char c,
                      u32 fg, u32 bg,
                      const struct font_desc *font,
                      int xres, int yres)
{
	int bpr = ((int)font->width + 7) / 8;
	const u8 *bits = (const u8 *)font->data + (int)c * (int)font->height * bpr;
	int row, col;

	for (row = 0; row < (int)font->height; row++) {
		for (col = 0; col < (int)font->width; col++) {
			bool lit = (bits[row * bpr + col / 8] >> (7 - col % 8)) & 1;
			if (!lit && bg == COLOR_NONE)
				continue;  // leave already-filled background
			PUT_PIXEL(sb, pitch, x + col, y + row,
			          lit ? fg : bg, xres, yres);
		}
	}
}

static int draw_str(struct drm_scanout_buffer *sb, u32 pitch,
                    int x, int y, const char *s,
                    u32 fg, u32 bg, const struct font_desc *font,
                    int max_x, int xres, int yres)
{
	while (*s && x + (int)font->width <= max_x) {
		draw_char(sb, pitch, x, y, (unsigned char)*s,
		          fg, bg, font, xres, yres);
		x += font->width;
		s++;
	}
	return x;
}

static int draw_wrapped(struct drm_scanout_buffer *sb, u32 pitch,
                        int x0, int y, const char *s,
                        u32 fg, u32 bg, const struct font_desc *font,
                        int max_x, int max_y, int xres, int yres)
{
	int cx = x0;

	while (*s && y + (int)font->height <= max_y) {
		if (*s == '\n') {
			cx = x0; y += font->height;
		} else if (cx + (int)font->width > max_x) {
			cx = x0; y += font->height;
			if (y + (int)font->height > max_y) break;
			draw_char(sb, pitch, cx, y, (unsigned char)*s,
			          fg, bg, font, xres, yres);
			cx += font->width;
		} else {
			draw_char(sb, pitch, cx, y, (unsigned char)*s,
			          fg, bg, font, xres, yres);
			cx += font->width;
		}
		s++;
	}
	return y + font->height;
}

static void draw_centered(struct drm_scanout_buffer *sb, u32 pitch,
                          int px, int pw, int y, const char *s,
                          u32 fg, u32 bg, const struct font_desc *font,
                          int xres, int yres)
{
	int tw = strlen(s) * (int)font->width;
	int x  = px + (pw - tw) / 2;

	draw_str(sb, pitch, x, y, s, fg, bg, font, px + pw, xres, yres);
}

// ---------------------------------------------------------------------------
// Main panic renderer — runs as a kmsg_dumper, AFTER drm_panic in the same
// kmsg_dump(KMSG_DUMP_PANIC) cycle, so we overdraw drm_panic's screen on the
// buffer captured by our kretprobe.

static void niko_render(const char *panic_msg)
{
	struct drm_scanout_buffer *sb;
	u32 pitch;
	int xres, yres;
	const struct font_desc *font_big, *font_sm;
	struct kmsg_dump_iter iter;
	int kidx = 0, kcount, i, y;
	char tmp[KMSG_LINE_W + 1];
	size_t len;
	int niko_x, niko_y;
	bool niko_ok;
	char panic_line[KMSG_LINE_W + 8];

	if (!sb_valid)
		return;

	sb    = &captured_sb;
	xres  = (int)sb->width;
	yres  = (int)sb->height;
	pitch = sb->pitch[0];

	if (!xres || !yres || !pitch)
		return;
	// Need either a set_pixel callback (VRAM path) or a valid map (CPU path).
	if (!sb->set_pixel) {
		if (sb->map[0].is_iomem && !sb->map[0].vaddr_iomem)
			return;
		if (!sb->map[0].is_iomem && !sb->map[0].vaddr)
			return;
	}

	font_big = find_font("ter_16x32");
	if (!font_big) font_big = find_font("VGA8x16");
	if (!font_big) return;

	font_sm = find_font("VGA8x16");
	if (!font_sm) font_sm = font_big;

	// Niko placement — computed first so the background fill can skip the box.
	niko_x = (LEFT_W - NIKO_W) / 2;
	niko_y = (yres - NIKO_H) / 2 - (int)font_big->height - 8;
	niko_ok = (niko_x >= 0 && niko_y >= 0 &&
	           niko_x + NIKO_W <= xres && niko_y + NIKO_H <= yres);

	// 1. Background. Niko is opaque, so fill around it (4 bands) instead of
	// painting ~186k pixels we'd immediately overwrite.
	if (niko_ok) {
		fill_rect(sb, pitch, 0, 0, xres, niko_y, COLOR_BG, xres, yres);
		fill_rect(sb, pitch, 0, niko_y + NIKO_H, xres,
		          yres - (niko_y + NIKO_H), COLOR_BG, xres, yres);
		fill_rect(sb, pitch, 0, niko_y, niko_x, NIKO_H, COLOR_BG, xres, yres);
		fill_rect(sb, pitch, niko_x + NIKO_W, niko_y,
		          xres - (niko_x + NIKO_W), NIKO_H, COLOR_BG, xres, yres);
	} else {
		fill_rect(sb, pitch, 0, 0, xres, yres, COLOR_BG, xres, yres);
	}

	// 2. Niko
	if (niko_ok)
		draw_niko(sb, pitch, niko_x, niko_y, xres, yres);

	// 3. Caption under Niko
	y = niko_y + NIKO_H + 16;
	draw_centered(sb, pitch, 0, LEFT_W, y, "I WASN'T READY.",
	              COLOR_PURPLE, COLOR_NONE, font_big, xres, yres);

	// 4. Vertical divider
	fill_rect(sb, pitch, DIVIDER_X, MARGIN_Y, 3, yres - MARGIN_Y * 2,
	          COLOR_PURPLE, xres, yres);

	// 5. Title — right panel
	y = MARGIN_Y + 4;
	draw_str(sb, pitch, RIGHT_X, y,
	         "THE WORLD MACHINE",
	         COLOR_PURPLE, COLOR_NONE, font_big,
	         xres - 10, xres, yres);

	y += font_big->height + 2;
	draw_str(sb, pitch, RIGHT_X, y,
	         "HAS CRASHED",
	         COLOR_PURPLE, COLOR_NONE, font_big,
	         xres - 10, xres, yres);

	// 6. Horizontal rule under title
	y += font_big->height + 8;
	fill_rect(sb, pitch, RIGHT_X, y, xres - 10 - RIGHT_X, 2,
	          COLOR_PURPLE, xres, yres);

	// 7. Panic reason
	y += 10;
	snprintf(panic_line, sizeof(panic_line), "PANIC: %s", panic_msg);
	y = draw_wrapped(sb, pitch, RIGHT_X, y, panic_line,
	                 COLOR_CYAN, COLOR_NONE, font_sm,
	                 xres - 10, y + font_sm->height * 4,
	                 xres, yres);

	// 8. Kernel log header
	y += font_sm->height / 2;
	draw_str(sb, pitch, RIGHT_X, y,
	         "--- KERNEL LOG ---",
	         COLOR_PURPLE, COLOR_NONE, font_sm,
	         xres - 10, xres, yres);
	y += font_sm->height + 2;

	// 9. Collect recent kmsg — only once. The log is frozen after panic, and
	// this avoids hammering printk locks on every panic_blink redraw.
	if (!kmsg_gathered) {
		kmsg_dump_rewind(&iter);
		while (kmsg_dump_get_line(&iter, false, tmp, KMSG_LINE_W, &len)) {
			if (len > 0 && tmp[len - 1] == '\n')
				len--;
			tmp[len] = '\0';
			memcpy(kbuf[kidx % KMSG_LINES], tmp, len + 1);
			kidx++;
		}
		kmsg_count = min(kidx, KMSG_LINES);
		kmsg_start = (kidx > KMSG_LINES) ? (kidx % KMSG_LINES) : 0;
		kmsg_gathered = true;
	}

	// 10. Draw kmsg
	kcount = kmsg_count;
	i      = kmsg_start;
	for (; kcount > 0 && y + (int)font_sm->height <= yres - 10; kcount--) {
		draw_str(sb, pitch, RIGHT_X, y,
		         kbuf[i % KMSG_LINES],
		         COLOR_WHITE, COLOR_NONE, font_sm,
		         xres - 10, xres, yres);
		y += font_sm->height;
		i++;
	}
}

// ---------------------------------------------------------------------------
// Hook points
//
// Two redraw paths, on purpose:
//   1. kmsg_dumper  — fires inside kmsg_dump(KMSG_DUMP_PANIC), same place as
//      drm_panic. Gives the earliest possible draw.
//   2. panic_blink  — called repeatedly in panic()'s final infinite loop,
//      AFTER console_unblank()/console_flush_on_panic(). This is the last code
//      to touch the screen, so it guarantees Niko wins regardless of dumper
//      ordering or any console repaint that slips past skip_panic.

static long (*old_panic_blink)(int state);
static int armed_count;

static void niko_dump(struct kmsg_dumper *dumper,
                      struct kmsg_dump_detail *detail)
{
	if (detail->reason != KMSG_DUMP_PANIC)
		return;

	if (detail->description) {
		strscpy(saved_desc, detail->description, sizeof(saved_desc));
	}

	niko_render(saved_desc);
}

static long niko_blink(int state)
{
	niko_render(saved_desc);
	return 0;
}

static struct kmsg_dumper niko_dumper = {
	.dump       = niko_dump,
	.max_reason = KMSG_DUMP_PANIC,
};

// GPU drivers are usually modules, so their symbols may not exist when we load
// (e.g. early boot via modules-load.d). Arm every driver whose symbol resolves
// now; a module notifier re-tries the rest once a new driver goes live. Whatever
// arms is the detected system — decided here, never during a panic.
static void try_arm_all(void)
{
	size_t i;

	for (i = 0; i < NIKO_NDRIVERS; i++) {
		struct niko_driver *drv = &niko_drivers[i];

		if (drv->armed)
			continue;

		drv->probe.handler        = scanout_ret;
		drv->probe.entry_handler  = scanout_entry;
		drv->probe.data_size      = sizeof(struct scanout_capture_data);
		drv->probe.maxactive      = 1;
		drv->probe.kp.symbol_name = drv->symbol;

		// Most failures just mean "this driver isn't loaded" — expected.
		if (register_kretprobe(&drv->probe) == 0) {
			drv->armed = true;
			armed_count++;
			pr_info("niko-panic: armed on %s [%s]\n",
			        drv->label, drv->symbol);
		}
	}
}

static int niko_mod_notify(struct notifier_block *nb,
                           unsigned long action, void *data)
{
	if (action == MODULE_STATE_LIVE && armed_count < (int)NIKO_NDRIVERS)
		try_arm_all();
	return NOTIFY_DONE;
}

static struct notifier_block niko_mod_nb = {
	.notifier_call = niko_mod_notify,
};

// ---------------------------------------------------------------------------

static int __init niko_panic_init(void)
{
	int ret;
	size_t i;

	register_module_notifier(&niko_mod_nb);

	try_arm_all();
	if (armed_count == 0)
		pr_warn("niko-panic: no GPU driver matched yet — will retry as "
		        "drivers load\n");

	ret = kmsg_dump_register(&niko_dumper);
	if (ret) {
		pr_warn("niko-panic: kmsg_dump_register failed (%d)\n", ret);
		unregister_module_notifier(&niko_mod_nb);
		for (i = 0; i < NIKO_NDRIVERS; i++)
			if (niko_drivers[i].armed)
				unregister_kretprobe(&niko_drivers[i].probe);
		return ret;
	}

	if (use_panic_blink) {
		old_panic_blink = panic_blink;
		panic_blink = niko_blink;
	}

	pr_info("niko-panic: loaded (%d driver(s) armed, skip_panic=%d panic_blink=%d)\n",
	        armed_count, use_skip_panic, use_panic_blink);
	return 0;
}

static void __exit niko_panic_exit(void)
{
	size_t i;

	if (use_panic_blink)
		panic_blink = old_panic_blink;
	kmsg_dump_unregister(&niko_dumper);
	unregister_module_notifier(&niko_mod_nb);
	for (i = 0; i < NIKO_NDRIVERS; i++)
		if (niko_drivers[i].armed)
			unregister_kretprobe(&niko_drivers[i].probe);
}

module_init(niko_panic_init);
module_exit(niko_panic_exit);
