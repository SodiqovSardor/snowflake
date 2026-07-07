#!/usr/bin/env bash
set -euo pipefail

# snowflake – zero-dependency file sharing
# One-liner: curl -fsSL https://raw.githubusercontent.com/SodiqovSardor/snowflake/main/install.sh | bash

REPO="SodiqovSardor/snowflake"
VERSION="${1:-latest}"
INSTALL_DIR="${INSTALL_DIR:-$HOME/.local/bin}"

# ── help ──────────────────────────────────────────────────────
if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
  echo "snowflake installer"
  echo ""
  echo "Usage: $0 [version]  (default: latest)"
  echo ""
  echo "  INSTALL_DIR=\$HOME/.local/bin  $0"
  echo "  INSTALL_DIR=/usr/local/bin    sudo $0"
  echo ""
  echo "  Build from source:"
  echo "    git clone https://github.com/$REPO"
  echo "    cd snowflake && make && make install"
  exit 0
fi

# ── platform detection ────────────────────────────────────────
ARCH=$(uname -m)
OS=$(uname -s | tr '[:upper:]' '[:lower:]')

case "$ARCH" in
  x86_64|amd64) ARCH="amd64" ;;
  aarch64|arm64) ARCH="arm64" ;;
  *) echo "unsupported arch: $ARCH"; exit 1 ;;
esac

case "$OS" in
  linux*) OS="linux" ;;
  darwin*) OS="darwin" ;;
  *) echo "unsupported OS: $OS"; exit 1 ;;
esac

# ── fetch release info ────────────────────────────────────────
if [ "$VERSION" = "latest" ]; then
  RELEASE_URL="https://api.github.com/repos/$REPO/releases/latest"
else
  RELEASE_URL="https://api.github.com/repos/$REPO/releases/tags/$VERSION"
fi

echo "  fetching release info..."

DOWNLOAD_URL=$(curl -fsSL "$RELEASE_URL" 2>/dev/null \
  | grep -oP '"browser_download_url":\s*"\K[^"]*(?=")' \
  | grep -i "snowflake-${OS}-${ARCH}" \
  | head -1) || true

if [ -z "$DOWNLOAD_URL" ]; then
  echo "  [info] no pre-built binary for ${OS}/${ARCH}"
  echo "  trying build from source..."
  if ! command -v g++ &>/dev/null; then
    echo "  [err] g++ not found. Install g++ or build manually"
    echo "  build from source: https://github.com/$REPO"
    exit 1
  fi
  SRC_DIR=$(mktemp -d /tmp/snowflake-build-XXXX)
  SRC_FILE="$SRC_DIR/src/client.cpp"
  mkdir -p "$SRC_DIR/src"
  echo "  downloading source..."
  curl -fsSL "https://raw.githubusercontent.com/$REPO/main/src/client.cpp" -o "$SRC_FILE" || {
    echo "  [err] source download failed. Build manually:"
    echo "    git clone https://github.com/$REPO && cd snowflake && make"
    exit 1
  }
  echo "  compiling..."
  g++ -std=c++17 -Os -s -o "$SRC_DIR/snowflake" "$SRC_FILE"
  TARGET_FILE="$SRC_DIR/snowflake"
  rm -rf "$SRC_DIR/src"
else
  # ── download binary ────────────────────────────────────────
  mkdir -p "$INSTALL_DIR"
  TARGET_FILE=$(mktemp /tmp/snowflake-dl-XXXX)

  echo "  downloading snowflake for ${OS}/${ARCH}..."
  curl -fsSL "$DOWNLOAD_URL" -o "$TARGET_FILE" || {
    echo "  [err] download failed"
    rm -f "$TARGET_FILE"
    exit 1
  }
fi

# ── install ──────────────────────────────────────────────────
mkdir -p "$INSTALL_DIR"
TARGET="$INSTALL_DIR/snowflake"
mv "$TARGET_FILE" "$TARGET"
chmod +x "$TARGET"

# ── PATH check ────────────────────────────────────────────────
if echo ":$PATH:" | grep -q ":$INSTALL_DIR:"; then
  echo ""
  echo "  \xe2\x9d\x84 snowflake installed to $TARGET"
  echo "  run: snowflake send . serve"
else
  echo ""
  echo "  \xe2\x9d\x84 snowflake installed to $TARGET"
  echo "  add to PATH: export PATH=\"$INSTALL_DIR:\$PATH\""
  echo "  or:  echo 'export PATH=\"$INSTALL_DIR:\$PATH\"' >> ~/.bashrc"
  echo "  then: source ~/.bashrc"
  echo "  then: snowflake send . serve"
fi
