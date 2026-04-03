#!/usr/bin/env bash
# =============================================================================
# cinebook_dev.sh  —  CineBook developer helper (MSYS2 / Linux / macOS)
# Place this in your project root (same folder as Makefile and main.c).
# Usage:  bash cinebook_dev.sh   or   ./cinebook_dev.sh
#
# Menu options
#   B  Build only          — runs make, shows compiler errors
#   R  Run (keep data)     — make then ./cinebook, existing .db files kept
#   F  Fresh start         — make + wipe data + reseed + run
#   S  Reseed only         — wipe data + reseed (no rebuild, no run)
#   T  Run tests           — build + run a headless smoke-test sequence
#   D  Clean build         — make clean + make (full recompile)
#   Q  Quit
# =============================================================================
set -euo pipefail

# ── Colours ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GRN='\033[0;32m'
YLW='\033[0;33m'
BLU='\033[0;34m'
CYN='\033[0;36m'
MAG='\033[0;35m'
BLD='\033[1m'
RST='\033[0m'

# ── Helpers ───────────────────────────────────────────────────────────────────
step()  { echo -e "${BLU}${BLD}==> $*${RST}"; }
ok()    { echo -e "${GRN}${BLD}  ✓ $*${RST}"; }
warn()  { echo -e "${YLW}${BLD}  ! $*${RST}"; }
fail()  { echo -e "${RED}${BLD}  ✗ $*${RST}"; }
ruler() { printf '%0.s─' {1..60}; echo; }

# ── Sanity-check: must be run from the project root ───────────────────────────
if [[ ! -f Makefile ]]; then
    fail "Makefile not found — run this script from the project root."
    exit 1
fi
# ── Create required directories ───────────────────────────────────────────────
make_dirs() {
    mkdir -p data/db data/idx exports
    ok "Runtime directories ready (data/db  data/idx  exports)"
}
# ── Build tool resolver ───────────────────────────────────────────────────────
resolve_make_cmd() {
    if command -v make >/dev/null 2>&1; then
        echo "make"
        return 0
    fi
    if command -v mingw32-make >/dev/null 2>&1; then
        echo "mingw32-make"
        return 0
    fi
    return 1
}

# ── Build ─────────────────────────────────────────────────────────────────────
do_build() {
    step "Building CineBook..."
    ruler

    local make_cmd
    make_cmd="$(resolve_make_cmd)" || {
        fail "No 'make' tool found. Install build tools first (Linux: sudo apt install build-essential)."
        return 1
    }

    if "$make_cmd" -j"$(nproc 2>/dev/null || echo 2)"; then
        ok "Build successful  →  ./cinebook"
    else
        fail "Build failed.  Fix errors above and try again."
        return 1
    fi
}

# ── Clean build ───────────────────────────────────────────────────────────────
do_clean_build() {
    step "Cleaning previous build..."
    local make_cmd
    make_cmd="$(resolve_make_cmd)" || {
        fail "No 'make' tool found. Install build tools first (Linux: sudo apt install build-essential)."
        return 1
    }
    "$make_cmd" clean 2>/dev/null || true
    ok "Cleaned."
    do_build
}

# ── Wipe all data files ───────────────────────────────────────────────────────
wipe_data() {
    step "Wiping data files..."
    local files=(data/db/*.db data/idx/*.idx data/wal.log)
    local failed=()
    for f in "${files[@]}"; do
        if [ -e "$f" ]; then
            rm -f "$f" 2>/dev/null || {
                # Try to kill processes using the file (Linux/macOS)
                if command -v fuser >/dev/null 2>&1; then
                    fuser -k "$f" 2>/dev/null
                elif command -v lsof >/dev/null 2>&1; then
                    lsof -t "$f" | xargs -r kill -9 2>/dev/null
                fi
                sleep 1
                rm -f "$f" 2>/dev/null || failed+=("$f")
            }
        fi
    done
    make_dirs
    if [ ${#failed[@]} -eq 0 ]; then
        ok "Data wiped."
    else
        warn "Some files could not be deleted (still in use): ${failed[*]}"
    fi
}

# ── Compile & run the seeder ──────────────────────────────────────────────────
do_seed() {
    step "Compiling seeder..."
    if gcc -std=c11 -Wall -O2 -o seed tools/seed.c 2>&1; then
        ok "Seeder compiled."
    else
        fail "Seeder compilation failed."
        return 1
    fi

    step "Running seeder..."
    ruler
    if ./seed; then
        ok "Database seeded."
    else
        fail "Seeder exited with an error."
        return 1
    fi
}

# ── Run the main binary ───────────────────────────────────────────────────────
do_run() {
    if [[ ! -f ./cinebook ]]; then
        warn "cinebook binary not found — building first."
        do_build || return 1
    fi

    echo
    echo -e "${MAG}${BLD}┌─────────────────────────────────────────┐${RST}"
    echo -e "${MAG}${BLD}│          Starting CineBook...           │${RST}"
    echo -e "${MAG}${BLD}└─────────────────────────────────────────┘${RST}"
    echo
    ./cinebook
    echo
    echo -e "${CYN}CineBook exited.${RST}"
}

# ── Smoke test (non-interactive stdin injection) ───────────────────────────────
# Sends a scripted sequence of inputs and checks for expected output lines.
do_smoke_test() {
    if [[ ! -f ./cinebook ]]; then
        warn "cinebook binary not found — building first."
        do_build || return 1
    fi

    step "Running smoke test (login as admin + exit)..."
    ruler

    # Inputs:  auth menu → 1 (Login) → phone → password → 8 (Logout) → 3 (Exit)
    local input_seq
    input_seq=$(printf '1\n9000000001\nadmin123\n8\n3\n')

    local output
    output=$(echo "$input_seq" | timeout 15 ./cinebook 2>&1) || true

    local passed=0 failed=0

    check_contains() {
        local label="$1" needle="$2"
        if echo "$output" | grep -q "$needle"; then
            ok "PASS  $label"
            ((passed++))
        else
            fail "FAIL  $label  (expected: '$needle')"
            ((failed++))
        fi
    }

    check_contains "Schema loaded"       "schema"
    check_contains "Buffer pool ready"   "pool\|storage\|buffer"
    check_contains "Auth menu shown"     "Login\|Sign Up\|CineBook"
    check_contains "Login accepted"      "Welcome\|Admin\|admin"
    check_contains "Admin menu shown"    "Admin Panel\|ADMIN\|Theatre"
    check_contains "Clean shutdown"      "Goodbye\|shutdown\|Shutting"

    ruler
    echo -e "${BLD}Results: ${GRN}${passed} passed${RST}  ${RED}${failed} failed${RST}"

    if [[ $failed -gt 0 ]]; then
        echo
        warn "Full output saved to  smoke_test_output.txt"
        echo "$output" > smoke_test_output.txt
        return 1
    fi
}

# ── Print elapsed time ────────────────────────────────────────────────────────
timer_start() { _T_START=$SECONDS; }
timer_end()   {
    local elapsed=$(( SECONDS - _T_START ))
    echo -e "${CYN}  Elapsed: ${elapsed}s${RST}"
}


# ── Simply Clean: Remove unnecessary and empty files ─────────────────────────
simply_clean() {
    step "Running Simply Clean (remove unnecessary and empty files)..."
    # Define patterns to keep (essential files and folders)
    keep_patterns=(
        "main.c" "Makefile" "README.md" "cinebook_dev.sh" "cinebook.conf.example" "LICENSE"
        "run.sh" "run.bat" "api_key.txt" "STORAGE_MONITORING.md"
        "src" "lib" "tools" "data/schema.cat"
        "src/*" "src/*/*" "lib/*" "tools/*"
        "data" "data/db" "data/idx" "exports" "build" "build/*" "main" "main/*"
        "*.h" "*.c" "*.cpp" "*.sh" "*.bat" "*.conf.example" "*.md"
    )

    # Find all files and filter out the ones to keep
    find . -type f | while read -r file; do
        keep=false
        for pat in "${keep_patterns[@]}"; do
            if [[ "$file" == ./$pat || "$file" == $pat ]]; then
                keep=true; break
            fi
            # Also allow for wildcard matches
            if [[ "$pat" == *"*"* && "$file" == ./${pat} ]]; then
                keep=true; break
            fi
        done
        # Remove log files, build artifacts, temp files, and files not matching keep patterns
        if ! $keep; then
            # Remove known non-essential files
            case "$file" in
                *.log|*.tmp|*.bak|*.swp|*.swo|*.o|*.out|*.exe|*.db|*.idx|*.seed|smoke_test_output.txt|exports/*|build/*|main/*)
                    rm -f "$file" && ok "Removed $file" ;;
                *)
                    # Remove empty files
                    if [ ! -s "$file" ]; then rm -f "$file" && ok "Removed empty $file"; fi
                    ;;
            esac
        fi
    done

    # Remove empty directories (except essential ones)
    find . -type d | while read -r dir; do
        case "$dir" in
            .|./src|./lib|./tools|./data|./data/db|./data/idx|./exports|./build|./main)
                ;; # keep
            *)
                if [ "$(ls -A "$dir")" == "" ]; then rmdir "$dir" && ok "Removed empty dir $dir"; fi
                ;;
        esac
    done
    ok "Simply Clean complete. Project is now bare bones."
}

# ── Main menu ─────────────────────────────────────────────────────────────────
show_menu() {
    clear
    echo
    echo -e "${CYN}${BLD}  ╔══════════════════════════════════════════╗${RST}"
    echo -e "${CYN}${BLD}  ║      CineBook — Developer Console        ║${RST}"
    echo -e "${CYN}${BLD}  ╚══════════════════════════════════════════╝${RST}"
    echo

    # Show binary & data status
    if [[ -f ./cinebook ]]; then
        echo -e "  Binary  : ${GRN}./cinebook  (exists)${RST}"
    else
        echo -e "  Binary  : ${RED}not built yet${RST}"
    fi

    # Declare variables inside the function
    db_count=
    db_files=()
    shopt -s nullglob
    db_files=(data/db/*.db)
    shopt -u nullglob
    db_count=${#db_files[@]}
    if (( db_count > 0 )); then
        echo -e "  Data    : ${GRN}${db_count} .db files present${RST}"
    else
        echo -e "  Data    : ${YLW}empty  (run F or S to seed)${RST}"
    fi

    echo
    echo -e "  ${BLD}B${RST}  Build only              (make)"
    echo -e "  ${BLD}R${RST}  Run   (keep data)       (make + ./cinebook)"
    echo -e "  ${BLD}F${RST}  Fresh start             (make + wipe + seed + run)"
    echo -e "  ${BLD}S${RST}  Reseed only             (wipe + seed)"
    echo -e "  ${BLD}T${RST}  Smoke test              (auto login / logout check)"
    echo -e "  ${BLD}D${RST}  Deep clean + build      (make clean + make)"
    echo -e "  ${BLD}C${RST}  Simply Clean            (remove unnecessary files)"
    echo -e "  ${BLD}Q${RST}  Quit"
    echo
    printf "  Choice: "
}

main() {
    make_dirs   # always ensure directories exist

    while true; do
        show_menu
        read -r choice
        echo

        timer_start

        case "${choice^^}" in   # convert to uppercase so b/B both work

            B)
                do_build
                timer_end
                echo; read -rp "  Press Enter to continue..." _
                ;;

            R)
                do_build && do_run
                timer_end
                echo; read -rp "  Press Enter to continue..." _
                ;;

            F)
                do_build || { echo; read -rp "  Press Enter..." _; continue; }
                wipe_data
                do_seed   || { echo; read -rp "  Press Enter..." _; continue; }
                do_run
                timer_end
                echo; read -rp "  Press Enter to continue..." _
                ;;

            S)
                wipe_data
                do_seed
                timer_end
                echo; read -rp "  Press Enter to continue..." _
                ;;

            T)
                do_smoke_test
                timer_end
                echo; read -rp "  Press Enter to continue..." _
                ;;

            D)
                do_clean_build
                timer_end
                echo; read -rp "  Press Enter to continue..." _
                ;;

            C)
                simply_clean
                timer_end
                echo; read -rp "  Press Enter to continue..." _
                ;;

            Q|"")
                echo -e "${CYN}  Goodbye.${RST}"
                exit 0
                ;;

            *)
                warn "Unknown option '${choice}' — use B R F S T D C Q"
                sleep 1
                ;;
        esac
    done
}

main "$@"
#!/usr/bin/env bash
# =============================================================================
# cinebook_dev.sh  —  CineBook developer helper (MSYS2 / Linux / macOS)
#
# Place this in your project root (same folder as Makefile and main.c).
# Usage:  bash cinebook_dev.sh   or   ./cinebook_dev.sh
#
# Menu options
#   B  Build only          — runs make, shows compiler errors
#   R  Run (keep data)     — make then ./cinebook, existing .db files kept
#   F  Fresh start         — make + wipe data + reseed + run
#   S  Reseed only         — wipe data + reseed (no rebuild, no run)
#   T  Run tests           — build + run a headless smoke-test sequence
#   D  Clean build         — make clean + make (full recompile)
#   Q  Quit
# =============================================================================

set -euo pipefail

# ── Colours ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GRN='\033[0;32m'
YLW='\033[0;33m'
BLU='\033[0;34m'
CYN='\033[0;36m'
MAG='\033[0;35m'
BLD='\033[1m'
RST='\033[0m'

# ── Helpers ───────────────────────────────────────────────────────────────────
step()  { echo -e "${BLU}${BLD}==> $*${RST}"; }
ok()    { echo -e "${GRN}${BLD}  ✓ $*${RST}"; }
warn()  { echo -e "${YLW}${BLD}  ! $*${RST}"; }
fail()  { echo -e "${RED}${BLD}  ✗ $*${RST}"; }
ruler() { printf '%0.s─' {1..60}; echo; }

# ── Sanity-check: must be run from the project root ───────────────────────────
if [[ ! -f Makefile ]]; then
    fail "Makefile not found — run this script from the project root."
    exit 1
fi

# ── Create required directories ───────────────────────────────────────────────
make_dirs() {
    mkdir -p data/db data/idx exports
    ok "Runtime directories ready (data/db  data/idx  exports)"
}

# ── Build tool resolver ───────────────────────────────────────────────────────
resolve_make_cmd() {
    if command -v make >/dev/null 2>&1; then
        echo "make"
        return 0
    fi
    if command -v mingw32-make >/dev/null 2>&1; then
        echo "mingw32-make"
        return 0
    fi
    return 1
}

# ── Build ─────────────────────────────────────────────────────────────────────
do_build() {
    step "Building CineBook..."
    ruler

    local make_cmd
    make_cmd="$(resolve_make_cmd)" || {
        fail "No 'make' tool found. Install build tools first (Linux: sudo apt install build-essential)."
        return 1
    }

    if "$make_cmd" -j"$(nproc 2>/dev/null || echo 2)"; then
        ok "Build successful  →  ./cinebook"
    else
        fail "Build failed.  Fix errors above and try again."
        return 1
    fi
}

# ── Clean build ───────────────────────────────────────────────────────────────
do_clean_build() {
    step "Cleaning previous build..."
    local make_cmd
    make_cmd="$(resolve_make_cmd)" || {
        fail "No 'make' tool found. Install build tools first (Linux: sudo apt install build-essential)."
        return 1
    }
    "$make_cmd" clean 2>/dev/null || true
    ok "Cleaned."
    do_build
}

# ── Wipe all data files ───────────────────────────────────────────────────────
wipe_data() {
    step "Wiping data files..."
    rm -f data/db/*.db data/idx/*.idx data/wal.log
    make_dirs
    ok "Data wiped."
}

# ── Compile & run the seeder ──────────────────────────────────────────────────
do_seed() {
    step "Compiling seeder..."
    if gcc -std=c11 -Wall -O2 -o seed tools/seed.c 2>&1; then
        ok "Seeder compiled."
    else
        fail "Seeder compilation failed."
        return 1
    fi

    step "Running seeder..."
    ruler
    if ./seed; then
        ok "Database seeded."
    else
        fail "Seeder exited with an error."
        return 1
    fi
}

# ── Run the main binary ───────────────────────────────────────────────────────
do_run() {
    if [[ ! -f ./cinebook ]]; then
        warn "cinebook binary not found — building first."
        do_build || return 1
    fi

    echo
    echo -e "${MAG}${BLD}┌─────────────────────────────────────────┐${RST}"
    echo -e "${MAG}${BLD}│          Starting CineBook...           │${RST}"
    echo -e "${MAG}${BLD}└─────────────────────────────────────────┘${RST}"
    echo
    ./cinebook
    echo
    echo -e "${CYN}CineBook exited.${RST}"
}

# ── Smoke test (non-interactive stdin injection) ───────────────────────────────
# Sends a scripted sequence of inputs and checks for expected output lines.
do_smoke_test() {
    if [[ ! -f ./cinebook ]]; then
        warn "cinebook binary not found — building first."
        do_build || return 1
    fi

    step "Running smoke test (login as admin + exit)..."
    ruler

    # Inputs:  auth menu → 1 (Login) → phone → password → 8 (Logout) → 3 (Exit)
    local input_seq
    input_seq=$(printf '1\n9000000001\nadmin123\n8\n3\n')

    local output
    output=$(echo "$input_seq" | timeout 15 ./cinebook 2>&1) || true

    local passed=0 failed=0

    check_contains() {
        local label="$1" needle="$2"
        if echo "$output" | grep -q "$needle"; then
            ok "PASS  $label"
            ((passed++))
        else
            fail "FAIL  $label  (expected: '$needle')"
            ((failed++))
        fi
    }

    check_contains "Schema loaded"       "schema"
    check_contains "Buffer pool ready"   "pool\|storage\|buffer"
    check_contains "Auth menu shown"     "Login\|Sign Up\|CineBook"
    check_contains "Login accepted"      "Welcome\|Admin\|admin"
    check_contains "Admin menu shown"    "Admin Panel\|ADMIN\|Theatre"
    check_contains "Clean shutdown"      "Goodbye\|shutdown\|Shutting"

    ruler
    echo -e "${BLD}Results: ${GRN}${passed} passed${RST}  ${RED}${failed} failed${RST}"

    if [[ $failed -gt 0 ]]; then
        echo
        warn "Full output saved to  smoke_test_output.txt"
        echo "$output" > smoke_test_output.txt
        return 1
    fi
}

# ── Print elapsed time ────────────────────────────────────────────────────────
timer_start() { _T_START=$SECONDS; }
timer_end()   {
    local elapsed=$(( SECONDS - _T_START ))
    echo -e "${CYN}  Elapsed: ${elapsed}s${RST}"
}

# ── Main menu ─────────────────────────────────────────────────────────────────
show_menu() {
    clear
    echo
    echo -e "${CYN}${BLD}  ╔══════════════════════════════════════════╗${RST}"
    echo -e "${CYN}${BLD}  ║      CineBook — Developer Console        ║${RST}"
    echo -e "${CYN}${BLD}  ╚══════════════════════════════════════════╝${RST}"
    echo

    # Show binary & data status
    if [[ -f ./cinebook ]]; then
        echo -e "  Binary  : ${GRN}./cinebook  (exists)${RST}"
    else
        echo -e "  Binary  : ${RED}not built yet${RST}"
    fi

    local db_count
    local db_files=()
    shopt -s nullglob
    db_files=(data/db/*.db)
    shopt -u nullglob
    db_count=${#db_files[@]}
    if (( db_count > 0 )); then
        echo -e "  Data    : ${GRN}${db_count} .db files present${RST}"
    else
        echo -e "  Data    : ${YLW}empty  (run F or S to seed)${RST}"
    fi

    echo
    echo -e "  ${BLD}B${RST}  Build only              (make)"
    echo -e "  ${BLD}R${RST}  Run   (keep data)       (make + ./cinebook)"
    echo -e "  ${BLD}F${RST}  Fresh start             (make + wipe + seed + run)"
    echo -e "  ${BLD}S${RST}  Reseed only             (wipe + seed)"
    echo -e "  ${BLD}T${RST}  Smoke test              (auto login / logout check)"
    echo -e "  ${BLD}D${RST}  Deep clean + build      (make clean + make)"
    echo -e "  ${BLD}Q${RST}  Quit"
    echo
    printf "  Choice: "
}

main() {
    make_dirs   # always ensure directories exist

    while true; do
        show_menu
        read -r choice
        echo

        timer_start

        case "${choice^^}" in   # convert to uppercase so b/B both work

            B)
                do_build
                timer_end
                echo; read -rp "  Press Enter to continue..." _
                ;;

            R)
                do_build && do_run
                timer_end
                echo; read -rp "  Press Enter to continue..." _
                ;;

            F)
                do_build || { echo; read -rp "  Press Enter..." _; continue; }
                wipe_data
                do_seed   || { echo; read -rp "  Press Enter..." _; continue; }
                do_run
                timer_end
                echo; read -rp "  Press Enter to continue..." _
                ;;

            S)
                wipe_data
                do_seed
                timer_end
                echo; read -rp "  Press Enter to continue..." _
                ;;

            T)
                do_smoke_test
                timer_end
                echo; read -rp "  Press Enter to continue..." _
                ;;

            D)
                do_clean_build
                timer_end
                echo; read -rp "  Press Enter to continue..." _
                ;;

            Q|"")
                echo -e "${CYN}  Goodbye.${RST}"
                exit 0
                ;;

            *)
                warn "Unknown option '${choice}' — use B R F S T D Q"
                sleep 1
                ;;
        esac
    done
}

main "$@"
#!/usr/bin/env bash
set -euo pipefail
RED='\033[0;31m'
GRN='\033[0;32m'
YLW='\033[0;33m'
BLU='\033[0;34m'
CYN='\033[0;36m'
MAG='\033[0;35m'
BLD='\033[1m'
RST='\033[0m'
step()  { echo -e "${BLU}${BLD}==> $*${RST}"; }
ok()    { echo -e "${GRN}${BLD}  ✓ $*${RST}"; }
warn()  { echo -e "${YLW}${BLD}  ! $*${RST}"; }
fail()  { echo -e "${RED}${BLD}  ✗ $*${RST}"; }
ruler() { printf '%0.s─' {1..60}; echo; }
has_lolcat() {
    command -v lolcat >/dev/null 2>&1
}
print_rainbow_block() {
    local text="$1"
    if has_lolcat; then
        printf "%b\n" "$text" | lolcat -f
    else
        echo -e "${CYN}${BLD}${text}${RST}"
    fi
}
if [[ ! -f Makefile ]]; then
    fail "Makefile not found — run this script from the project root."
    exit 1
fi
make_dirs() {
    mkdir -p data/db data/idx exports
    ok "Runtime directories ready (data/db  data/idx  exports)"
}
resolve_make_cmd() {
    if command -v make >/dev/null 2>&1; then
        echo "make"
        return 0
    fi
    if command -v mingw32-make >/dev/null 2>&1; then
        echo "mingw32-make"
        return 0
    fi
    return 1
}
do_build() {
    step "Building CineBook..."
    ruler
    local make_cmd; make_cmd=$(resolve_make_cmd) || { fail "No 'make' tool found."; exit 1; }
    if ! "$make_cmd" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"; then
        fail "Build failed."
        exit 1
    fi
    ok "Build complete."
}
do_clean_build() {
    step "Cleaning previous build..."
    local make_cmd; make_cmd=$(resolve_make_cmd) || { fail "No 'make' tool found."; exit 1; }
    "$make_cmd" clean
    do_build
}
wipe_data() {
    step "Wiping data files..."
    rm -f data/db/*.db data/idx/*.idx data/wal.log
    ok "Data wiped."
}
do_seed() {
    step "Compiling seeder..."
    if gcc -std=c11 -Wall -O2 -o seed tools/seed.c 2>&1; then
        ok "Seeder compiled."
    else
        fail "Seeder compilation failed."
        exit 1
    fi
    step "Seeding database..."
    if ./seed; then
        ok "Database seeded."
    else
        fail "Database seeding failed."
        exit 1
    fi
}
do_run() {
    if [[ ! -f ./cinebook ]]; then
        warn "cinebook binary not found — building first."
        do_build
    fi
    step "Running CineBook..."
    ./cinebook
    echo -e "${CYN}CineBook exited.${RST}"
}
do_smoke_test() {
    if [[ ! -f ./cinebook ]]; then
        warn "cinebook binary not found — building first."
        do_build
    fi
    step "Running smoke test (login as admin + exit)..."
    ruler
    local input_seq
    input_seq=$(printf '1\n9000000001\nadmin123\n8\n3\n')
    if echo "$input_seq" | ./cinebook | grep -q "Welcome, Admin CineBook"; then
        ok "Smoke test passed."
    else
        fail "Smoke test failed."
        exit 1
    fi
}
timer_start() { _T_START=$SECONDS; }
timer_end()   {
    local elapsed=$(( SECONDS - _T_START ))
    echo -e "${CYN}  Elapsed: ${elapsed}s${RST}"
}
show_menu() {
    clear
    echo
    print_rainbow_block "  ╔══════════════════════════════════════════╗"
    print_rainbow_block "  ║      CineBook — Developer Console        ║"
    print_rainbow_block "  ╚══════════════════════════════════════════╝"
    echo
    if [[ -f ./cinebook ]]; then
        echo -e "  Binary  : ${GRN}./cinebook  (exists)${RST}"
    else
        echo -e "  Binary  : ${RED}./cinebook  (not built)${RST}"
    fi
    if [[ -d data/db && $(ls -1q data/db/*.db 2>/dev/null | wc -l) -gt 0 ]]; then
        echo -e "  Data    : ${GRN}data/db/*.db  (present)${RST}"
    else
        echo -e "  Data    : ${RED}data/db/*.db  (missing)${RST}"
    fi
    echo
    echo "  [B] Build   [R] Run   [F] Fresh Start   [S] Reseed   [T] Test   [D] Clean   [Q] Quit"
    echo
}
main() {
    make_dirs
    while true; do
        show_menu
        read -rp "  Select option: " choice
        timer_start
        case "${choice^^}" in
            B)
                do_build
                ;;
            R)
                do_run
                ;;
            F)
                do_clean_build
                wipe_data
                do_seed
                do_run
                ;;
            S)
                wipe_data
                do_seed
                ;;
            T)
                do_clean_build
                do_smoke_test
                ;;
            D)
                do_clean_build
                ;;
            Q)
                echo "Exiting."
                exit 0
                ;;
            *)
                warn "Invalid option."
                ;;
        esac
        timer_end
        echo
        read -rp "  Press Enter to continue..." _
    done
}
main
