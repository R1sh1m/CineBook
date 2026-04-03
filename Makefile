# ─────────────────────────────────────────────────────────────────────────────
# Makefile — CineBook
# BACSE104  |  Structured & Object-Oriented Programming
#
# Compilers : gcc  for all .c  files  (C11)
#             g++  for all .cpp files (C++17)
#             g++  as the linker (required — links C++ stdlib + libcurl)
#
# Layout:
#   src/engine/   storage.c record.c schema.c index.c txn.c query.c
#   src/auth/     auth.c session.c
#   src/logic/    pricing.c refund.c payment.c promos.c location.c
#   src/ui/       ui_browse.c ui_booking.c ui_cancel.c ui_account.c
#                 ui_admin.c ui_dashboard.c
#   src/reports/  reports.cpp
#   lib/          cJSON.c
#   main.c        (project root)
#
# All object files land in build/
# ─────────────────────────────────────────────────────────────────────────────

# ── Version ───────────────────────────────────────────────────────────────────
VERSION := 1.5.0

# ── OS detection ──────────────────────────────────────────────────────────────
UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)

# ── Toolchain (per OS) ────────────────────────────────────────────────────────
ifneq (,$(filter MINGW% MSYS% Windows%,$(UNAME_S)))
SHELL    := C:/msys64/usr/bin/sh.exe
MKDIR_P  := C:/msys64/usr/bin/mkdir -p
RM_F     := C:/msys64/usr/bin/rm -f
RM_RF    := C:/msys64/usr/bin/rm -rf
CC       := C:/msys64/mingw64/bin/gcc
CXX      := C:/msys64/mingw64/bin/g++

MSYS2_MINGW64 := C:/msys64/mingw64
CURL_INC_PATH := $(MSYS2_MINGW64)/include
CURL_LIB_PATH := $(MSYS2_MINGW64)/lib
CURL_INC      := -I$(CURL_INC_PATH)
CURL_LIB      := -L$(CURL_LIB_PATH)
LIBS          := $(CURL_LIB) -lcurl -lcrypto -liphlpapi -lws2_32
TARGET        := cinebook.exe

else ifeq ($(UNAME_S),Darwin)
SHELL    := /bin/sh
MKDIR_P  := mkdir -p
RM_F     := rm -f
RM_RF    := rm -rf
CC       := gcc
CXX      := g++

CURL_PREFIX   := $(shell brew --prefix curl 2>/dev/null)
ifeq ($(CURL_PREFIX),)
CURL_INC_PATH := /usr/local/include
CURL_LIB_PATH := /usr/local/lib
else
CURL_INC_PATH := $(CURL_PREFIX)/include
CURL_LIB_PATH := $(CURL_PREFIX)/lib
endif
CURL_INC      := -I$(CURL_INC_PATH)
CURL_LIB      := -L$(CURL_LIB_PATH)
LIBS          := $(CURL_LIB) -lcurl -lcrypto
TARGET        := cinebook

else ifeq ($(UNAME_S),Linux)
SHELL    := /bin/sh
MKDIR_P  := mkdir -p
RM_F     := rm -f
RM_RF    := rm -rf
CC       := gcc
CXX      := g++

CURL_INC_PATH :=
CURL_LIB_PATH :=
CURL_INC      :=
CURL_LIB      :=
LIBS          := -lcurl -lcrypto
TARGET        := cinebook

else
$(error Unsupported platform '$(UNAME_S)')
endif

CFLAGS   := -Wall -Wextra -std=c11   -O2 $(CURL_INC)
CXXFLAGS := -Wall -Wextra -std=c++17 -O2 $(CURL_INC)

# ── Include paths ─────────────────────────────────────────────────────────────
INCLUDES := -I src/engine \
            -I src/auth   \
            -I src/logic  \
            -I src/ui     \
            -I src/reports \
            -I src/crypto \
            -I src/setup  \
            -I lib

# ── Source files ─────────────────────────────────────────────────────────────
C_SRCS := main.c \
          src/engine/storage.c \
          src/engine/record.c  \
          src/engine/schema.c  \
          src/engine/index.c   \
          src/engine/txn.c     \
          src/engine/query.c   \
          src/engine/integrity.c \
          src/engine/compact.c \
          src/auth/auth.c      \
          src/auth/session.c   \
          src/logic/pricing.c  \
          src/logic/refund.c   \
          src/logic/payment.c  \
          src/logic/promos.c   \
          src/logic/location.c \
          src/ui/ui_browse.c   \
          src/ui/ui_booking.c  \
          src/ui/ui_cancel.c   \
          src/ui/ui_account.c  \
          src/ui/ui_admin.c    \
          src/ui/ui_dashboard.c \
          src/ui/ui_utils.c    \
          src/ui/messages.c    \
          src/ui/banner.c      \
          src/crypto/keystore.c \
          src/setup/wizard.c   \
          lib/cJSON.c

CPP_SRCS := src/reports/reports.cpp

# ── Object files (all land in build/) ────────────────────────────────────────
C_OBJS   := $(patsubst %.c,   build/%.o, $(C_SRCS))
CPP_OBJS := $(patsubst %.cpp, build/%.o, $(CPP_SRCS))
ALL_OBJS := $(C_OBJS) $(CPP_OBJS)

# ─────────────────────────────────────────────────────────────────────────────
# Default target
# ─────────────────────────────────────────────────────────────────────────────
.PHONY: all dirs run clean list check-deps install-deps-mac install-deps-linux
.PHONY: install test clean-db debug package help
all: dirs $(TARGET)

dirs:
	$(MKDIR_P) build/main
	$(MKDIR_P) build/src/engine
	$(MKDIR_P) build/src/auth
	$(MKDIR_P) build/src/logic
	$(MKDIR_P) build/src/ui
	$(MKDIR_P) build/src/reports
	$(MKDIR_P) build/src/crypto
	$(MKDIR_P) build/src/setup
	$(MKDIR_P) build/lib
	$(MKDIR_P) data/db
	$(MKDIR_P) data/idx
	$(MKDIR_P) exports

# ── Link — always use g++ so the C++ runtime and libcurl resolve cleanly ─────
$(TARGET): $(ALL_OBJS)
	$(CXX) -o $@ $^ $(LIBS)
	@echo ""
	@echo "  Build complete → ./$(TARGET)"
	@echo ""

# ─────────────────────────────────────────────────────────────────────────────
# Compile rules
# ─────────────────────────────────────────────────────────────────────────────
build/%.o: %.c
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

build/%.o: %.cpp
	@$(MKDIR_P) $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# ─────────────────────────────────────────────────────────────────────────────
# Dependency checks — Verify all required tools and libraries
# ─────────────────────────────────────────────────────────────────────────────
check-deps:
	@echo ""
	@echo "  Checking dependencies for $(UNAME_S)..."
	@echo "  ────────────────────────────────────────"
	@echo ""
ifeq ($(UNAME_S),Darwin)
	@printf "  gcc      : "
	@if command -v gcc >/dev/null 2>&1; then \
		echo "✓ $$(gcc --version | head -n1)"; \
	else \
		echo "✗ NOT FOUND"; exit 1; \
	fi
	@printf "  g++      : "
	@if command -v g++ >/dev/null 2>&1; then \
		echo "✓ $$(g++ --version | head -n1)"; \
	else \
		echo "✗ NOT FOUND"; exit 1; \
	fi
	@printf "  libcurl  : "
	@if [ -f "$(CURL_LIB_PATH)/libcurl.dylib" ] || [ -f "/usr/lib/libcurl.dylib" ]; then \
		CURL_VER=$$(curl-config --version 2>/dev/null | cut -d' ' -f2 || echo "unknown"); \
		echo "✓ $$CURL_VER"; \
	else \
		echo "✗ NOT FOUND — Run: make install-deps-mac"; exit 1; \
	fi
	@printf "  ruby     : "
	@if command -v ruby >/dev/null 2>&1; then \
		echo "✓ $$(ruby --version | cut -d' ' -f2)"; \
	else \
		echo "⚠ NOT FOUND (optional for lolcat)"; \
	fi
	@printf "  lolcat   : "
	@if command -v lolcat >/dev/null 2>&1; then \
		echo "✓ installed"; \
	else \
		echo "⚠ NOT FOUND (optional rainbow text)"; \
	fi
	@printf "  brew     : "
	@if command -v brew >/dev/null 2>&1; then \
		echo "✓ installed"; \
	else \
		echo "✗ NOT FOUND — Required for macOS"; exit 1; \
	fi
else ifeq ($(UNAME_S),Linux)
	@printf "  gcc      : "
	@if command -v gcc >/dev/null 2>&1; then \
		echo "✓ $$(gcc --version | head -n1)"; \
	else \
		echo "✗ NOT FOUND"; exit 1; \
	fi
	@printf "  g++      : "
	@if command -v g++ >/dev/null 2>&1; then \
		echo "✓ $$(g++ --version | head -n1)"; \
	else \
		echo "✗ NOT FOUND"; exit 1; \
	fi
	@printf "  libcurl  : "
	@if ldconfig -p 2>/dev/null | grep -q libcurl || pkg-config --exists libcurl 2>/dev/null; then \
		CURL_VER=$$(curl-config --version 2>/dev/null | cut -d' ' -f2 || echo "installed"); \
		echo "✓ $$CURL_VER"; \
	else \
		echo "✗ NOT FOUND — Run: make install-deps-linux"; exit 1; \
	fi
	@printf "  openssl  : "
	@if command -v openssl >/dev/null 2>&1; then \
		SSL_VER=$$(openssl version | cut -d' ' -f2); \
		echo "✓ $$SSL_VER"; \
	else \
		echo "⚠ NOT FOUND (recommended)"; \
	fi
	@printf "  ruby     : "
	@if command -v ruby >/dev/null 2>&1; then \
		echo "✓ $$(ruby --version | cut -d' ' -f2)"; \
	else \
		echo "⚠ NOT FOUND (optional for lolcat)"; \
	fi
	@printf "  lolcat   : "
	@if command -v lolcat >/dev/null 2>&1; then \
		echo "✓ installed"; \
	else \
		echo "⚠ NOT FOUND (optional rainbow text)"; \
	fi
else
	@printf "  gcc      : "
	@if [ -x "$(CC)" ]; then \
		echo "✓ $$($(CC) --version | head -n1)"; \
	else \
		echo "✗ NOT FOUND at $(CC)"; exit 1; \
	fi
	@printf "  g++      : "
	@if [ -x "$(CXX)" ]; then \
		echo "✓ $$($(CXX) --version | head -n1)"; \
	else \
		echo "✗ NOT FOUND at $(CXX)"; exit 1; \
	fi
	@printf "  libcurl  : "
	@if [ -f "$(CURL_LIB_PATH)/libcurl.a" ] || [ -f "$(CURL_LIB_PATH)/libcurl.dll.a" ]; then \
		echo "✓ found in $(CURL_LIB_PATH)"; \
	else \
		echo "✗ NOT FOUND in $(CURL_LIB_PATH)"; exit 1; \
	fi
	@printf "  MSYS2    : "
	@if [ -d "C:/msys64" ]; then \
		echo "✓ C:/msys64"; \
	else \
		echo "✗ NOT FOUND"; exit 1; \
	fi
endif
	@echo ""
	@echo "  ✓ All required dependencies present"
	@echo ""

install-deps-mac:
	@brew install curl

install-deps-linux:
	@if command -v apt-get >/dev/null 2>&1; then \
		sudo apt-get update && sudo apt-get install -y libcurl4-openssl-dev g++ gcc; \
	elif command -v dnf >/dev/null 2>&1; then \
		sudo dnf install -y libcurl-devel gcc gcc-c++; \
	else \
		echo "Neither apt-get nor dnf found. Install libcurl dev package, gcc, and g++ manually."; \
		exit 1; \
	fi

# ─────────────────────────────────────────────────────────────────────────────
# run — create runtime directories then execute
# ─────────────────────────────────────────────────────────────────────────────
run: $(TARGET)
	@$(MKDIR_P) data/db data/idx exports
	./$(TARGET)

# ─────────────────────────────────────────────────────────────────────────────
# clean
# ─────────────────────────────────────────────────────────────────────────────
clean:
	@$(RM_RF) build/
	@$(RM_F) $(TARGET) $(TARGET).exe
	@echo "  Cleaned build artefacts."

# ─────────────────────────────────────────────────────────────────────────────
# Convenience: show the file list that will be compiled
# ─────────────────────────────────────────────────────────────────────────────
list:
	@echo "C sources   ($(words $(C_SRCS))):"
	@for s in $(C_SRCS); do echo "  $$s"; done
	@echo ""
	@echo "C++ sources ($(words $(CPP_SRCS))):"
	@for s in $(CPP_SRCS); do echo "  $$s"; done

# ─────────────────────────────────────────────────────────────────────────────
# install — Install binary to system path
# ─────────────────────────────────────────────────────────────────────────────
install: $(TARGET)
ifeq ($(UNAME_S),Darwin)
	@echo "Installing to /usr/local/bin/cinebook..."
	@install -m 755 $(TARGET) /usr/local/bin/cinebook
	@echo "  ✓ Installed: /usr/local/bin/cinebook"
else ifeq ($(UNAME_S),Linux)
	@echo "Installing to /usr/local/bin/cinebook..."
	@sudo install -m 755 $(TARGET) /usr/local/bin/cinebook
	@echo "  ✓ Installed: /usr/local/bin/cinebook"
else
	@echo "  ⚠ Install target not supported on Windows."
	@echo "  Add $(TARGET) to your PATH manually."
endif

# ─────────────────────────────────────────────────────────────────────────────
# test — Run integrity tests
# ─────────────────────────────────────────────────────────────────────────────
test: $(TARGET)
	@echo ""
	@echo "  Running integrity tests..."
	@echo ""
	@if [ -f test_storage_monitoring.c ]; then \
		echo "  Compiling test_storage_monitoring..."; \
		$(CC) $(CFLAGS) $(INCLUDES) -o test_storage_monitoring test_storage_monitoring.c \
			src/engine/storage.c src/engine/record.c src/engine/schema.c \
			src/engine/index.c src/engine/txn.c lib/cJSON.c $(LIBS) && \
		echo "  Running test_storage_monitoring..." && \
		./test_storage_monitoring && \
		echo "" && \
		echo "  ✓ All tests passed."; \
	else \
		echo "  ℹ No test files found. Skipping."; \
	fi

# ─────────────────────────────────────────────────────────────────────────────
# clean-db — Reset database (remove all .db and .idx files)
# ─────────────────────────────────────────────────────────────────────────────
clean-db:
	@echo "  Resetting database..."
	@$(RM_F) data/db/*.db data/idx/*.idx data/wal.log 2>/dev/null || true
	@echo "  ✓ Database reset. Run 'make run' to reseed."

# ─────────────────────────────────────────────────────────────────────────────
# debug — Build with debug symbols and no optimization
# ─────────────────────────────────────────────────────────────────────────────
debug: CFLAGS := -Wall -Wextra -std=c11 -g -O0 -DDEBUG $(CURL_INC)
debug: CXXFLAGS := -Wall -Wextra -std=c++17 -g -O0 -DDEBUG $(CURL_INC)
debug: clean dirs $(TARGET)
	@echo ""
	@echo "  ✓ Debug build complete with -g -O0 -DDEBUG"
	@echo ""

# ─────────────────────────────────────────────────────────────────────────────
# package — Create distributable tarball with pre-seeded database
# ─────────────────────────────────────────────────────────────────────────────
package: $(TARGET)
	@echo ""
	@echo "  Creating release package..."
	@echo ""
	@$(MKDIR_P) package/cinebook-$(VERSION)
	@$(MKDIR_P) package/cinebook-$(VERSION)/data/db
	@$(MKDIR_P) package/cinebook-$(VERSION)/data/idx
	@$(MKDIR_P) package/cinebook-$(VERSION)/exports
	@echo "  Copying binary..."
	@cp $(TARGET) package/cinebook-$(VERSION)/
	@echo "  Copying schema..."
	@cp schema.cat package/cinebook-$(VERSION)/ 2>/dev/null || true
	@echo "  Copying README..."
	@cp README.md package/cinebook-$(VERSION)/ 2>/dev/null || true
	@cp LICENSE package/cinebook-$(VERSION)/ 2>/dev/null || true
	@echo "  Seeding database..."
	@if [ -f tools/seed.c ]; then \
		$(CC) -std=c11 -Wall -O2 -o package/seed tools/seed.c && \
		cd package && ./seed && rm -f seed && cd ..; \
	fi
	@echo "  Copying seeded data..."
	@cp -r data/db/*.db package/cinebook-$(VERSION)/data/db/ 2>/dev/null || true
	@echo "  Creating tarball..."
	@cd package && tar -czf cinebook-v$(VERSION).tar.gz cinebook-$(VERSION)
	@mv package/cinebook-v$(VERSION).tar.gz .
	@$(RM_RF) package
	@echo ""
	@echo "  ✓ Package created: cinebook-v$(VERSION).tar.gz"
	@echo ""

# ─────────────────────────────────────────────────────────────────────────────
# help — Show available targets
# ─────────────────────────────────────────────────────────────────────────────
help:
	@echo ""
	@echo "  CineBook v$(VERSION) — Available Make Targets"
	@echo "  ════════════════════════════════════════════════"
	@echo ""
	@echo "  all          Build the project (default)"
	@echo "  run          Build and run CineBook"
	@echo "  clean        Remove build artifacts"
	@echo "  clean-db     Reset database (removes .db and .idx files)"
	@echo "  debug        Build with debug symbols (-g -O0 -DDEBUG)"
	@echo "  test         Run integrity tests"
	@echo "  install      Install binary to /usr/local/bin"
	@echo "  package      Create distributable tarball (v$(VERSION))"
	@echo "  check-deps   Verify all dependencies are present"
	@echo "  list         Show all source files"
	@echo "  help         Show this help message"
	@echo ""
