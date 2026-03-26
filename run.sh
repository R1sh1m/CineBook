#!/usr/bin/env bash
# run.sh — CineBook auto-setup launcher (Linux / macOS / MSYS2)
# ─────────────────────────────────────────────────────────────
# Usage:  bash run.sh
# ─────────────────────────────────────────────────────────────

set -euo pipefail

RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[0;33m'
CYN='\033[0;36m'; BLD='\033[1m'; RST='\033[0m'

ok()   { echo -e "${GRN}${BLD}  ✓ $*${RST}"; }
warn() { echo -e "${YLW}${BLD}  ! $*${RST}"; }
die()  { echo -e "${RED}${BLD}  ✗ $*${RST}"; exit 1; }

echo
echo -e "${CYN}${BLD}  CineBook — Auto Setup & Launch${RST}"
echo "  ────────────────────────────────────"

# ── 1. Detect OS / environment ─────────────────────────────────────────────
OS="$(uname -s)"
case "$OS" in
    Linux*)     PLATFORM="linux" ;;
    Darwin*)    PLATFORM="macos" ;;
    MINGW*|MSYS*|CYGWIN*)  PLATFORM="msys2" ;;
    *)          die "Unsupported platform: $OS" ;;
esac
ok "Platform: $PLATFORM"

# ── 2. Install missing dependencies ────────────────────────────────────────
case "$PLATFORM" in
    linux)
        if ! command -v gcc &>/dev/null; then
            warn "gcc not found — installing build-essential..."
            sudo apt-get update -qq && sudo apt-get install -y -qq build-essential
        fi
        if ! dpkg -l libcurl4-openssl-dev &>/dev/null 2>&1; then
            warn "libcurl-dev not found — installing..."
            sudo apt-get install -y -qq libcurl4-openssl-dev 2>/dev/null || \
            sudo dnf install -y -q libcurl-devel 2>/dev/null || \
            warn "Could not install libcurl automatically. Please run: sudo apt install libcurl4-openssl-dev"
        fi
        ;;
    macos)
        if ! command -v brew &>/dev/null; then
            die "Homebrew is required on macOS. Install it from https://brew.sh and re-run."
        fi
        if ! brew list curl &>/dev/null 2>&1; then
            warn "curl not found via Homebrew — installing..."
            brew install curl
        fi
        ;;
    msys2)
        if ! command -v gcc &>/dev/null; then
            warn "gcc not found — installing MinGW toolchain..."
            pacman -S --needed --noconfirm mingw-w64-x86_64-gcc mingw-w64-x86_64-curl-openssl make
        fi
        ;;
esac
ok "Dependencies ready"

# ── 3. Create runtime directories ──────────────────────────────────────────
mkdir -p data/db data/idx exports
ok "Runtime directories ready"

# ── 4. Build ────────────────────────────────────────────────────────────────
echo
echo -e "${CYN}  Building CineBook...${RST}"
if ! make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"; then
    die "Build failed. Check errors above."
fi
ok "Build complete"

# ── 5. Seed database (only if empty) ───────────────────────────────────────
DB_COUNT=$(ls data/db/*.db 2>/dev/null | wc -l || echo 0)
if (( DB_COUNT == 0 )); then
    echo
    echo -e "${CYN}  Seeding database (first run)...${RST}"
    gcc -std=c11 -Wall -O2 -o seed tools/seed.c || die "Seeder compilation failed."
    ./seed || die "Seeder failed."
    ok "Database seeded"
else
    ok "Database already seeded ($DB_COUNT tables found)"
fi

# ── 6. Launch ───────────────────────────────────────────────────────────────
echo
echo -e "${CYN}${BLD}  Launching CineBook...${RST}"
echo "  Admin login → phone: 9000000001 | password: admin123"
echo
./cinebook
