#!/bin/bash
# Configure niko_panic to load on every boot, AFTER amdgpu.
set -e

# Ensure amdgpu is loaded before niko_panic so the kretprobe symbol exists.
echo "softdep niko_panic pre: amdgpu" | sudo tee /etc/modprobe.d/niko-panic.conf

# Auto-load at boot.
echo "niko_panic" | sudo tee /etc/modules-load.d/niko-panic.conf

echo "Auto-load + softdep configured. Active on next boot."
