# niko-panic

A loadable kernel module that **replaces the Linux kernel panic screen with a custom image + a clean panel of the last dmesg lines** — no kernel recompile, no patching `drm_panic.c`. It rides on top of the in-kernel `drm_panic` infrastructure, draws over it the instant a panic happens, and survives via DKMS across kernel updates.

Themed here as [OneShot](https://store.steampowered.com/app/420530/OneShot/)'s "The World Machine has crashed" — but the art is yours to swap.

```
        ┌───────────────────────────┬─────────────────────────────────┐
        │                           ║  THE WORLD MACHINE              │
        │        [ your image ]     ║  HAS CRASHED                    │
        │                           ║  ─────────────────────────────  │
        │                           ║  PANIC: <reason>                │
        │      I WASN'T READY.      ║  --- KERNEL LOG ---             │
        │                           ║  <last N lines of dmesg>        │
        └───────────────────────────┴─────────────────────────────────┘
```

## Why this is more than a reskin

Modern Linux (≥6.12) has `drm_panic`, which paints a QR code or the tail of dmesg when the kernel dies. This module hijacks that path at runtime:

1. A **kretprobe** on the GPU driver's `get_scanout_buffer` captures the live framebuffer the moment `drm_panic` grabs it.
2. A **kmsg_dumper** (registered after `drm_panic`, so it runs after it) overdraws the whole screen with our layout.
3. It sets the fbdev's **`skip_panic`** flag so the console doesn't repaint the dmesg over us.

The interesting part is the framebuffer access. amdgpu (and others) expose **two** ways to write pixels at panic time:

- **CPU-mapped buffer** (`map[0]`) — integrated GPUs / APUs, where the framebuffer lives in CPU-visible memory.
- **`set_pixel()` MMIO callback** — **discrete GPUs**, where the scanout buffer is in VRAM with `NO_CPU_ACCESS`, so the only way to poke it during a panic is one indirect MMIO write per pixel.

Most "custom panic screen" hacks only handle the mapped case and silently do nothing on a discrete card. This one handles both. (Ask me how I know.)

## Supported systems

One module, auto-detecting at load time. It registers a probe for every known driver symbol; whichever driver is actually loaded arms. Detection never happens during a panic.

| System | DRM driver | Hooked symbol |
|---|---|---|
| **AMD Radeon/RDNA dGPU + Ryzen APU** | `amdgpu` | `amdgpu_display_get_scanout_buffer` |
| Intel iGPU + Arc dGPU | `i915` | `intel_get_scanout_buffer` |
| NVIDIA (open, Maxwell GM10x+) | `nouveau` | `nv50_wndw_get_scanout_buffer` |
| VESA / EFI / simpledrm firmware fb | `sysfb` | `drm_sysfb_plane_helper_get_scanout_buffer` |
| ARM SoC (i.MX / R-Car / TI) | various | `drm_fb_dma_get_scanout_buffer` |
| ASPEED BMC (servers) | `ast` | `ast_primary_plane_helper_get_scanout_buffer` |
| Matrox G200 BMC (servers) | `mgag200` | `mgag200_primary_plane_helper_get_scanout_buffer` |
| virtio-gpu (QEMU/KVM) | `virtio-gpu` | `virtio_drm_get_scanout_buffer` |
| bochs / stdvga (QEMU) | `bochs` | `bochs_primary_plane_helper_get_scanout_buffer` |
| Hyper-V (VM) | `hyperv_drm` | `hyperv_plane_get_scanout_buffer` |

> ⚠️ **Tested on real hardware: AMD only** (a Radeon RX 6700 XT, the author's card — the discrete `set_pixel` path). The other symbols are verified against the mainline source and share the identical callback signature and the same `set_pixel`/`map` model, so they *should* work — but they're untested on metal. When you load the module, `dmesg | grep niko` prints `armed on <system>` for whatever matched, so you'll know instantly if the symbol resolved on your box. Reports welcome.

## Requirements

- Linux **≥ 6.12** with `CONFIG_DRM_PANIC=y` and `CONFIG_KPROBES=y` / `CONFIG_KRETPROBES=y`
- A console font compiled in (`CONFIG_FONT_TER16x32` or `CONFIG_FONT_8x16`)
- Kernel headers for your running kernel
- **Build the module with the same compiler as your kernel.** If your kernel is Clang-built (e.g. CachyOS), build with `LLVM=1` (already the default in the `Makefile`).
- Build-time only, for baking the image: **ImageMagick** + **Python 3**

## Build & install

```sh
# 1. Bake your image into a C header (see "The image" below)
./gen_image.sh /path/to/your-image.png

# 2. Build
make

# 3. Quick test (loads the freshly built .ko)
sudo insmod ./niko_panic.ko
dmesg | grep niko          # should show "armed on <your GPU>"

# 4. Permanent install (DKMS — auto-rebuilds on kernel updates)
sudo ./install.sh
sudo ./autoload.sh         # load at boot, after the GPU driver
```

Trigger a test panic (**this will crash your machine** — save your work):

```sh
echo c | sudo tee /proc/sysrq-trigger
```

## The image

`gen_image.sh` turns any image into `niko_image.h`:

```
your-image ──[ImageMagick: scale + recolor background]──▶ raw RGB
           ──[Python: emit C hex array]──▶ niko_image.h
           ──[#include + clang]──▶ baked into niko_panic.ko
```

It scales the image and replaces the background color so you don't get a white flashbang in the middle of a dark panic screen. Nothing external is needed at runtime — the pixels are compiled into the module. You supply your own art; none is shipped with this repo (see *Legal*).

## Configuration

Module parameters (set at `modprobe`/`insmod` time):

| Param | Default | Meaning |
|---|---|---|
| `use_skip_panic` | `1` | Set fbdev `skip_panic` so the console doesn't repaint dmesg over the screen |
| `use_panic_blink` | `0` | Redraw from `panic_blink` for persistence. Leave **off** on discrete GPUs — per-pixel MMIO is too slow to repaint 5×/sec. Useful on CPU-mapped (APU/iGPU) setups. |

## Screenshots

How the hell do you expect me to take a screenshot of a kernel panic? It's a *kernel panic.* The machine is dead. There is no userspace. There is no screenshot tool. There is only a phone camera, a slightly crooked angle, and the glow of `#0a0010` on your face at 2am.

Point a camera at your monitor like our ancestors did.

## How it works (the gory details)

- `drm_panic` is a `kmsg_dumper` registered per-plane by each DRM driver at boot. `kmsg_dump()` walks dumpers in registration order; ours registers later, so it runs *after* `drm_panic` in the same `KMSG_DUMP_PANIC` cycle and overdraws it.
- The kretprobe's entry handler captures `plane` (arg 1) and `sb` (arg 2); the return handler copies the populated `drm_scanout_buffer` and flips `skip_panic`.
- Pixel writes go through `set_pixel()` when the driver provides it (discrete VRAM), else direct writes to the mapped buffer.
- Text is drawn with a transparent background (only lit glyph pixels), and the area behind the opaque image isn't filled — both to cut the number of slow per-pixel MMIO writes on discrete cards.

## Legal

GPL-2.0 (required for kernel modules — see [`LICENSE`](LICENSE)).

**No artwork is included.** Niko / OneShot is © Future Cat LLC; bring your own image. This repo ships only code and the build tooling.

Built for the love of cursed kernel hacks.
