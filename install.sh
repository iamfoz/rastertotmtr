#!/bin/sh
# Installs the universal rastertotmtr filter over Epson's x86_64-only one.
# The original bundle is backed up beside it as rastertotmtr.app.x86_64.bak
# (run with "--uninstall" to restore it).
set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
FILTER_DIR=/Library/Printers/EPSON/tmprinter/filter
DEST="$FILTER_DIR/rastertotmtr.app"
BACKUP="$FILTER_DIR/rastertotmtr.app.x86_64.bak"
BIN="$DEST/Contents/MacOS/rastertotmtr"

if [ "$(id -u)" -ne 0 ]; then
    exec sudo "$0" "$@"
fi

if [ "${1:-}" = "--uninstall" ]; then
    [ -d "$BACKUP" ] || { echo "No backup at $BACKUP" >&2; exit 1; }
    rm -rf "$DEST"
    mv "$BACKUP" "$DEST"
    echo "Restored original filter."
    exit 0
fi

if [ ! -f "$DIR/rastertotmtr.app/Contents/MacOS/rastertotmtr" ]; then
    echo "Build first: make" >&2
    exit 1
fi

if [ -d "$DEST" ] && [ ! -d "$BACKUP" ]; then
    mv "$DEST" "$BACKUP"
    echo "Backed up original to $BACKUP"
fi

mkdir -p "$FILTER_DIR"
rm -rf "$DEST"
cp -R "$DIR/rastertotmtr.app" "$DEST"
chown -R root:wheel "$DEST"
chmod -R u=rwX,go=rX "$DEST"

echo "Installed: $BIN"
file "$BIN"
echo
echo "The PPD's *cupsFilter line already points at this path, so existing"
echo "EPSON TM queues use it on the next print job. No re-add needed."
