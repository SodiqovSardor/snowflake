#!/usr/bin/env bash
# Build snowflake binary for release
set -euo pipefail

cd "$(dirname "$0")"
OUTDIR="dist"
mkdir -p "$OUTDIR"

OS=$(uname -s | tr '[:upper:]' '[:lower:]')
ARCH=$(uname -m)
case "$ARCH" in
  x86_64|amd64) ARCH="amd64" ;;
  aarch64|arm64) ARCH="arm64" ;;
esac

echo "  building snowflake-${OS}-${ARCH} ..."
g++ -std=c++17 -Os -s -static -o "$OUTDIR/snowflake-${OS}-${ARCH}" src/client.cpp
strip "$OUTDIR/snowflake-${OS}-${ARCH}" 2>/dev/null || true
xz -f "$OUTDIR/snowflake-${OS}-${ARCH}"

echo ""
echo "  dist/snowflake-${OS}-${ARCH}.xz  ($(ls -lh "dist/snowflake-${OS}-${ARCH}.xz" | awk '{print $5}'))"
echo ""
echo "  To publish:"
echo "    gh release create v1.0 dist/* --title 'v1.0' --notes '...'"
