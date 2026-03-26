#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

OS_NAME="$(uname -s 2>/dev/null || echo unknown)"

ensure_dirs() {
  mkdir -p "data/db" "data/idx" "exports"
}

check_macos_deps() {
  if ! command -v brew >/dev/null 2>&1; then
    echo "[run] Homebrew not found. Install Homebrew first: https://brew.sh"
  fi

  local brew_prefix=""
  if command -v brew >/dev/null 2>&1; then
    brew_prefix="$(brew --prefix curl 2>/dev/null || true)"
  fi

  if [[ -n "$brew_prefix" ]]; then
    export DYLD_LIBRARY_PATH="$brew_prefix/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
    export CPPFLAGS="-I$brew_prefix/include ${CPPFLAGS:-}"
    export LDFLAGS="-L$brew_prefix/lib ${LDFLAGS:-}"
  elif [[ -d "/usr/local/lib" ]]; then
    export DYLD_LIBRARY_PATH="/usr/local/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
  fi

  if ! command -v curl >/dev/null 2>&1; then
    echo "[run] curl/libcurl appears missing. Install with: brew install curl"
  fi
}

check_linux_deps() {
  if command -v ldconfig >/dev/null 2>&1; then
    if ! ldconfig -p 2>/dev/null | grep -q "libcurl"; then
      if command -v apt-get >/dev/null 2>&1; then
        echo "[run] libcurl dev package may be missing. Install:"
        echo "      sudo apt-get install -y libcurl4-openssl-dev"
      elif command -v dnf >/dev/null 2>&1; then
        echo "[run] libcurl dev package may be missing. Install:"
        echo "      sudo dnf install -y libcurl-devel"
      fi
    fi
  fi

  if command -v apt-get >/dev/null 2>&1; then
    :
  elif command -v dnf >/dev/null 2>&1; then
    :
  else
    echo "[run] Neither apt nor dnf detected. Ensure gcc/g++ and libcurl dev are installed."
  fi

  if [[ -d "/usr/local/lib" ]]; then
    export LD_LIBRARY_PATH="/usr/local/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  fi
}

build_if_needed() {
  if [[ ! -x "./cinebook" ]]; then
    echo "[run] ./cinebook not found. Building..."
    make
  fi
}

seed_if_needed() {
  if [[ ! -f "data/db/users.db" ]]; then
    echo "[run] users.db not found. Seeding database..."
    rm -f "./seed"
    gcc -std=c11 -Wall -o seed tools/seed.c
    ./seed
    rm -f "./seed"
  fi
}

run_app() {
  ./cinebook || {
    code=$?
    echo "[run] cinebook failed to start or exited with code $code."
    echo "[run] Check README for dependency/runtime setup."
    exit "$code"
  }
}

ensure_dirs

case "$OS_NAME" in
  Darwin)
    check_macos_deps
    ;;
  Linux)
    check_linux_deps
    ;;
  *)
    echo "[run] Unsupported OS for run.sh: $OS_NAME"
    echo "[run] Use run.bat on Windows, or run manually."
    exit 1
    ;;
esac

build_if_needed
seed_if_needed
run_app