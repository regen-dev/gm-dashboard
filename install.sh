#!/bin/bash
# GM Dashboard installer — places files into ~/.local/
set -euo pipefail

PREFIX="${PREFIX:-$HOME/.local}"
CFGDIR="$HOME/.config/gm-dashboard"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "Installing GM Dashboard to $PREFIX ..."

# Binaries
install -Dm755 "$SCRIPT_DIR/bin/gm-dashboard" "$PREFIX/bin/gm-dashboard"
install -Dm755 "$SCRIPT_DIR/bin/gm-viewer"    "$PREFIX/bin/gm-viewer"
install -Dm755 "$SCRIPT_DIR/bin/gm-sign"      "$PREFIX/bin/gm-sign"
install -Dm755 "$SCRIPT_DIR/bin/gm-config"    "$PREFIX/bin/gm-config"

# Plugin header
install -Dm644 "$SCRIPT_DIR/include/gm-dashboard/gm-plugin.h" \
  "$PREFIX/include/gm-dashboard/gm-plugin.h"

# Shared data
install -Dm644 "$SCRIPT_DIR/share/gm-dashboard/base.html" \
  "$PREFIX/share/gm-dashboard/base.html"

# Plugins
install -dm755 "$PREFIX/lib/gm-dashboard"
for f in "$SCRIPT_DIR"/lib/gm-dashboard/*.so; do
  [ -f "$f" ] && install -m755 "$f" "$PREFIX/lib/gm-dashboard/"
done
for f in "$SCRIPT_DIR"/lib/gm-dashboard/*.html; do
  [ -f "$f" ] && install -m644 "$f" "$PREFIX/lib/gm-dashboard/"
done

# Icons + desktop file
install -Dm644 "$SCRIPT_DIR/share/icons/gm-dashboard.svg" \
  "$PREFIX/share/icons/hicolor/scalable/apps/gm-dashboard.svg"
install -Dm644 "$SCRIPT_DIR/share/icons/gm-dashboard.png" \
  "$PREFIX/share/icons/hicolor/256x256/apps/gm-dashboard.png"
install -Dm644 "$SCRIPT_DIR/share/applications/gm-dashboard.desktop" \
  "$PREFIX/share/applications/gm-dashboard.desktop"
gtk-update-icon-cache -q "$PREFIX/share/icons/hicolor" 2>/dev/null || true
update-desktop-database -q "$PREFIX/share/applications" 2>/dev/null || true

# Config (don't overwrite existing)
install -dm700 "$CFGDIR"
if [ ! -f "$CFGDIR/config" ]; then
  install -m600 "$SCRIPT_DIR/share/gm-dashboard/config.example" "$CFGDIR/config"
fi

# Generate signing keypair if missing
if [ ! -f "$CFGDIR/marketplace.pub" ]; then
  echo "Generating Ed25519 signing keypair..."
  "$PREFIX/bin/gm-sign" keygen
fi

# Sign plugins
echo "Signing plugins..."
for so in "$PREFIX"/lib/gm-dashboard/*.so; do
  "$PREFIX/bin/gm-sign" sign "$so"
done

echo ""
echo "Done! Make sure ~/.local/bin is in your PATH, then run:"
echo "  gm-dashboard"
