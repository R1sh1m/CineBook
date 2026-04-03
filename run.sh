#!/usr/bin/env bash
# run.sh — CineBook auto-setup launcher (Linux / macOS / MSYS2)
# Usage:  bash run.sh

set -euo pipefail

RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[0;33m'
CYN='\033[0;36m'; BLD='\033[1m'; RST='\033[0m'

ok()   { echo -e "${GRN}${BLD}  ✓ $*${RST}"; }
warn() { echo -e "${YLW}${BLD}  ! $*${RST}"; }
die()  { echo -e "${RED}${BLD}  ✗ $*${RST}"; exit 1; }

has_lolcat() {
    command -v lolcat &>/dev/null
}

print_rainbow_block() {
    local text="$1"
    if has_lolcat; then
        printf "%b\n" "$text" | lolcat -f
    else
        echo -e "${CYN}${BLD}${text}${RST}"
    fi
}

resolve_make_cmd() {
    if command -v make &>/dev/null; then
        echo "make"
        return 0
    fi
    if command -v mingw32-make &>/dev/null; then
        echo "mingw32-make"
        return 0
    fi
    return 1
}

echo
print_rainbow_block "  CineBook — Auto Setup & Launch"
echo "  ────────────────────────────────────"

OS="$(uname -s)"
case "$OS" in
    Linux*)     PLATFORM="linux" ;;
    Darwin*)    PLATFORM="macos" ;;
    MINGW*|MSYS*|CYGWIN*)  PLATFORM="msys2" ;;
    *)          die "Unsupported platform: $OS" ;;
esac
ok "Platform: $PLATFORM"

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
        # Install Ruby + lolcat for colorful banner
        if ! command -v lolcat &>/dev/null; then
            if ! command -v ruby &>/dev/null; then
                warn "Ruby not found — installing for lolcat support..."
                sudo apt-get install -y -qq ruby-full 2>/dev/null || \
                sudo dnf install -y -q ruby 2>/dev/null || \
                warn "Could not install Ruby. lolcat will be unavailable."
            fi
            if command -v ruby &>/dev/null && ! command -v lolcat &>/dev/null; then
                warn "Installing lolcat for rainbow banner..."
                gem install lolcat --user-install 2>/dev/null || \
                sudo gem install lolcat 2>/dev/null || \
                warn "Could not install lolcat. Using fallback colors."
            fi
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
        # Install lolcat for colorful banner
        if ! command -v lolcat &>/dev/null; then
            if ! command -v ruby &>/dev/null; then
                warn "Ruby not found — installing for lolcat support..."
                brew install ruby
            fi
            if command -v ruby &>/dev/null; then
                warn "Installing lolcat for rainbow banner..."
                gem install lolcat --user-install 2>/dev/null || \
                brew install lolcat 2>/dev/null || \
                warn "Could not install lolcat. Using fallback colors."
            fi
        fi
        ;;
    msys2)
        if ! command -v gcc &>/dev/null; then
            warn "gcc not found — installing MinGW toolchain..."
            pacman -S --needed --noconfirm mingw-w64-x86_64-gcc mingw-w64-x86_64-curl-openssl make
        fi
        # Install Ruby + lolcat for colorful banner
        if ! command -v lolcat &>/dev/null; then
            if ! command -v ruby &>/dev/null; then
                warn "Ruby not found — installing for lolcat support..."
                pacman -S --needed --noconfirm ruby
            fi
            if command -v ruby &>/dev/null; then
                warn "Installing lolcat for rainbow banner..."
                gem install lolcat --user-install 2>/dev/null || \
                warn "Could not install lolcat. Using fallback colors."
            fi
        fi
        ;;
esac
ok "Dependencies ready"

mkdir -p data/db data/idx exports
ok "Runtime directories ready"

echo
echo -e "${CYN}  Building CineBook...${RST}"
MAKE_CMD="$(resolve_make_cmd)" || die "No 'make' tool found. Install build tools (Linux: sudo apt install build-essential)."
if ! "$MAKE_CMD" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"; then
    die "Build failed. Check errors above."
fi
ok "Build complete"

shopt -s nullglob
db_files=(data/db/*.db)
shopt -u nullglob
DB_COUNT=${#db_files[@]}
if (( DB_COUNT == 0 )); then
    echo
    echo -e "${CYN}  Seeding database (first run)...${RST}"
    gcc -std=c11 -Wall -O2 -o seed tools/seed.c || die "Seeder compilation failed."
    ./seed || die "Seeder failed."
    ok "Database seeded"
else
    ok "Database already seeded ($DB_COUNT tables found)"
fi

echo
print_rainbow_block "  Launching CineBook..."
echo "  Admin login → phone: 9000000001 | password: admin123"
echo
./cinebook
