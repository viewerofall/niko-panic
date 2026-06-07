#!/bin/bash
# Install niko-panic as a DKMS module (auto-rebuilds on kernel updates).
set -e
SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
PKG="niko-panic"
VER="1.0"
SRC="/usr/src/$PKG-$VER"

# Generate image header if missing
if [ ! -f "$SCRIPT_DIR/niko_image.h" ]; then
    echo "Generating niko_image.h..."
    bash "$SCRIPT_DIR/gen_image.sh"
fi

# Fully purge any previous version first. Without this, DKMS happily reuses a
# stale build cache and modprobe loads an old binary (this bit us hard).
echo "Purging old DKMS state..."
sudo dkms remove "$PKG/$VER" --all 2>/dev/null || true
sudo rm -rf "/var/lib/dkms/$PKG" "$SRC"

echo "Installing fresh source to $SRC..."
sudo mkdir -p "$SRC"
sudo cp "$SCRIPT_DIR/niko_panic.c" \
        "$SCRIPT_DIR/niko_image.h" \
        "$SCRIPT_DIR/Makefile" \
        "$SCRIPT_DIR/dkms.conf" \
        "$SRC/"

sudo dkms add     -m "$PKG" -v "$VER"
sudo dkms build   -m "$PKG" -v "$VER"
sudo dkms install -m "$PKG" -v "$VER" --force

echo ""
echo "DKMS install complete:"
sudo dkms status "$PKG"
echo ""
echo "Next: run ./autoload.sh to configure boot loading with GPU auto-detection."
