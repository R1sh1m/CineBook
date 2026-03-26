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
LIBS          := $(CURL_LIB) -lcurl
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
LIBS          := $(CURL_LIB) -lcurl
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
LIBS          := -lcurl
TARGET        := cinebook

else
$(error Unsupported platform '$(UNAME_S)')
endif

CFLAGS   := -Wall -Wextra -std=c11   -g $(CURL_INC)
CXXFLAGS := -Wall -Wextra -std=c++17 -g $(CURL_INC)

# ── Include paths ─────────────────────────────────────────────────────────────
INCLUDES := -I src/engine \
            -I src/auth   \
            -I src/logic  \
            -I src/ui     \
            -I src/reports \
            -I lib

# ── Source files ─────────────────────────────────────────────────────────────
C_SRCS := main.c \
          src/engine/storage.c \
          src/engine/record.c  \
          src/engine/schema.c  \
          src/engine/index.c   \
          src/engine/txn.c     \
          src/engine/query.c   \
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
all: dirs $(TARGET)

dirs:
	$(MKDIR_P) build/main
	$(MKDIR_P) build/src/engine
	$(MKDIR_P) build/src/auth
	$(MKDIR_P) build/src/logic
	$(MKDIR_P) build/src/ui
	$(MKDIR_P) build/src/reports
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
# Dependency checks
# ─────────────────────────────────────────────────────────────────────────────
check-deps:
ifeq ($(UNAME_S),Darwin)
	@if ! command -v brew >/dev/null 2>&1; then \
		echo "Missing Homebrew. Install from https://brew.sh"; \
		exit 1; \
	fi
	@if [ ! -f "$(CURL_LIB_PATH)/libcurl.dylib" ] && [ ! -f "/usr/lib/libcurl.dylib" ]; then \
		echo "libcurl not found. Run: make install-deps-mac"; \
		exit 1; \
	fi
	@echo "Dependencies OK (macOS)."
else ifeq ($(UNAME_S),Linux)
	@if ! ldconfig -p 2>/dev/null | grep -q libcurl && ! pkg-config --exists libcurl 2>/dev/null; then \
		echo "libcurl not found. Install with apt or dnf."; \
		echo "Run: make install-deps-linux"; \
		exit 1; \
	fi
	@echo "Dependencies OK (Linux)."
else
	@if [ ! -x "$(CC)" ]; then \
		echo "MSYS2 MinGW gcc not found at $(CC)."; \
		echo "Install MSYS2 and MinGW-w64 toolchain."; \
		exit 1; \
	fi
	@if [ ! -f "$(CURL_LIB_PATH)/libcurl.a" ] && [ ! -f "$(CURL_LIB_PATH)/libcurl.dll.a" ]; then \
		echo "libcurl not found in $(CURL_LIB_PATH)."; \
		exit 1; \
	fi
	@echo "Dependencies OK (Windows/MSYS2)."
endif

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