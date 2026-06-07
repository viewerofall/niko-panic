#!/bin/bash
# Configure niko_panic to load on every boot, after the correct GPU driver.
# Detects which driver was actually armed by reading dmesg after a probe load.
set -e

MODPROBE_CONF=/etc/modprobe.d/niko-panic.conf
MODULES_CONF=/etc/modules-load.d/niko-panic.conf

# Symbol -> kernel module name.
# simpledrm is intentionally absent: it's a firmware fb active before the real
# GPU driver loads and gets replaced at boot, so no softdep is needed for it.
declare -A SYM_TO_MOD=(
    [amdgpu_display_get_scanout_buffer]="amdgpu"
    [intel_get_scanout_buffer]="i915"
    [nv50_wndw_get_scanout_buffer]="nouveau"
    [ast_primary_plane_helper_get_scanout_buffer]="ast"
    [mgag200_primary_plane_helper_get_scanout_buffer]="mgag200"
    [virtio_drm_get_scanout_buffer]="virtio-gpu"
    [bochs_primary_plane_helper_get_scanout_buffer]="bochs"
    [hyperv_plane_get_scanout_buffer]="hv_vmbus"
)

# Probe the module temporarily so we can read which symbols armed.
# If it's already loaded (e.g. called after install.sh), skip.
ALREADY_LOADED=false
if lsmod | grep -q "^niko_panic"; then
    ALREADY_LOADED=true
else
    echo "Loading niko_panic temporarily to detect GPU driver..."
    if ! sudo modprobe niko_panic 2>/dev/null; then
        # Module not installed yet via DKMS — try insmod from cwd.
        if [ -f "./niko_panic.ko" ]; then
            sudo insmod ./niko_panic.ko
        else
            echo "ERROR: niko_panic not loaded and no .ko found in current directory." >&2
            echo "Run install.sh first, or build with 'make' and re-run from the build dir." >&2
            exit 1
        fi
    fi
fi

# Give dmesg a moment to flush, then extract armed symbols.
sleep 0.2
ARMED_SYMS=$(dmesg | grep "niko-panic: armed on" | grep -oP '\[\K[^\]]+' | sort -u)

if [ -z "$ARMED_SYMS" ]; then
    echo "WARNING: No armed symbols found in dmesg." >&2
    echo "The module loaded but no GPU driver matched. softdep will be omitted." >&2
    echo "niko_panic will still load at boot via modules-load.d and retry via" >&2
    echo "the module notifier when your GPU driver comes up." >&2
    DEPS=""
else
    DEPS=""
    for sym in $ARMED_SYMS; do
        mod="${SYM_TO_MOD[$sym]}"
        if [ -n "$mod" ]; then
            echo "  Detected: $sym -> $mod"
            DEPS="$DEPS $mod"
        else
            # simpledrm or unknown — no softdep needed, just log it.
            echo "  Detected: $sym (no softdep needed, skipping)"
        fi
    done
    DEPS="${DEPS# }"  # trim leading space
fi

# Unload the temporary probe load if we loaded it ourselves.
if [ "$ALREADY_LOADED" = false ]; then
    sudo rmmod niko_panic 2>/dev/null || true
fi

# Write modprobe config.
if [ -n "$DEPS" ]; then
    SOFTDEP_LINE="softdep niko_panic pre: $DEPS"
    echo "Writing: $SOFTDEP_LINE"
    echo "$SOFTDEP_LINE" | sudo tee "$MODPROBE_CONF" > /dev/null
else
    # No softdep — clear any stale config so we don't hardcode the wrong driver.
    sudo rm -f "$MODPROBE_CONF"
    echo "No softdep written (no matching driver or only simpledrm)."
fi

# Auto-load at boot.
echo "niko_panic" | sudo tee "$MODULES_CONF" > /dev/null

echo ""
echo "Auto-load configured. Active on next boot."
[ -n "$DEPS" ] && echo "softdep: niko_panic pre: $DEPS"
