# ttyd-client -- a companion command line client for ttyd
#
# The canonical build system is CMake (see CMakeLists.txt and the CI
# workflows). This Makefile is a thin convenience wrapper around it, plus a
# dependency free "bootstrap" build that only needs a C compiler.
#
# TLS is always enabled: every target links OpenSSL, and the static builds
# vendor a target specific static OpenSSL automatically. Set VENDOR=0 to use a
# prebuilt static OpenSSL (OPENSSL_ROOT_DIR) instead.
#
# Examples:
#   make                  # configure + build with CMake (Release, with TLS)
#   make cc               # build directly with $(CC), no CMake needed
#   make static           # fully static musl build with TLS (vendors OpenSSL)
#   make static VENDOR=0  # fully static musl build with a prebuilt OpenSSL
#   make install          # install into $(PREFIX)
#   make format           # clang-format the sources
#   make clean

CC ?= cc
PREFIX ?= /usr/local
BUILD_DIR ?= build
CMAKE ?= cmake
BUILD_TYPE ?= Release
VENDOR ?= 1
OPENSSL_VERSION ?= 3.6.1
OPENSSL_PREFIX ?= $(BUILD_DIR)/openssl
OPENSSL_WORK ?= $(BUILD_DIR)/openssl-build
OPENSSL_ROOT_DIR ?=

# The static target defaults to musl-gcc but honors CC overrides:
#   make static CC=aarch64-linux-musl-gcc
#   make static CC=x86_64-w64-mingw32-gcc
STATIC_CC ?= $(if $(filter default,$(origin CC)),musl-gcc,$(CC))
STATIC_TRIPLE := $(shell $(STATIC_CC) -dumpmachine 2>/dev/null)
ifeq ($(STATIC_TRIPLE),)
  STATIC_TRIPLE := unknown
endif
WINDOWS := $(findstring mingw,$(STATIC_TRIPLE))
STATIC_EXE := $(if $(WINDOWS),.exe,)

ifeq ($(VENDOR),0)
  VENDOR_DEP :=
  STATIC_OPENSSL_ROOT := $(if $(OPENSSL_ROOT_DIR),$(abspath $(OPENSSL_ROOT_DIR)),)
else
  VENDOR_DEP := vendor-openssl
  STATIC_OPENSSL_ROOT := $(abspath $(OPENSSL_PREFIX))
endif

ifneq ($(STATIC_OPENSSL_ROOT),)
  STATIC_OPENSSL_CMAKE := -DOPENSSL_ROOT_DIR="$(STATIC_OPENSSL_ROOT)" -DOPENSSL_USE_STATIC_LIBS=TRUE
  STATIC_PKG_CONFIG := PKG_CONFIG_LIBDIR="$(STATIC_OPENSSL_ROOT)/lib/pkgconfig:$(STATIC_OPENSSL_ROOT)/lib64/pkgconfig"
else
  STATIC_OPENSSL_CMAKE := -DOPENSSL_USE_STATIC_LIBS=TRUE
  STATIC_PKG_CONFIG :=
endif

STATIC_PLATFORM := $(if $(WINDOWS),-DCMAKE_SYSTEM_NAME=Windows,)
STATIC_LDFLAGS := -static $(if $(WINDOWS),,-no-pie) -Wl,-s -Wl,--gc-sections

SRC := $(wildcard src/*.c)
HDR := $(wildcard src/*.h)

.PHONY: all build configure cc static vendor-openssl clean distclean install uninstall format help

all: build

help:
	@echo "targets:"
	@echo "  all/build          configure and build with CMake (default, with TLS)"
	@echo "  cc                 build with \$$(CC) without CMake"
	@echo "  static             static musl build with TLS (vendors OpenSSL)"
	@echo "                     CC=<cross>-gcc and CC=<triple>-w64-mingw32-gcc work"
	@echo "                     VENDOR=0 uses a prebuilt OpenSSL (OPENSSL_ROOT_DIR)"
	@echo "  install            install into \$$(PREFIX)"
	@echo "  format             clang-format the sources"
	@echo "  clean              remove the build directory"
	@echo ""
	@echo "variables:"
	@echo "  OPENSSL_VERSION    OpenSSL version to vendor (default: $(OPENSSL_VERSION))"
	@echo "  OPENSSL_ROOT_DIR   prebuilt static OpenSSL prefix (VENDOR=0)"

configure:
	@mkdir -p "$(BUILD_DIR)"
	@$(CMAKE) -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
	    -DCMAKE_INSTALL_PREFIX="$(PREFIX)"

build: configure
	@$(CMAKE) --build "$(BUILD_DIR)" --parallel

# A plain compiler build, useful when CMake is not available
cc:
	@$(CC) -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE \
	    -Isrc -o ttyd-client $(SRC) -lssl -lcrypto
	@echo "built ./ttyd-client"

# Download and build a static OpenSSL for the compiler used by the static
# target. The stamp makes repeat builds a no-op and forces a rebuild when the
# version or target changes.
vendor-openssl:
	@set -e; state="$(OPENSSL_VERSION):$(STATIC_TRIPLE)"; \
	if [ -f "$(OPENSSL_PREFIX)/.vendor-state" ] && \
	   [ "$$(cat "$(OPENSSL_PREFIX)/.vendor-state")" = "$$state" ]; then \
	    echo "OpenSSL $(OPENSSL_VERSION) for $(STATIC_TRIPLE) is already built"; \
	else \
	    rm -rf "$(OPENSSL_PREFIX)" "$(OPENSSL_WORK)"; \
	    mkdir -p "$(OPENSSL_WORK)"; \
	    OPENSSL_CFLAGS_EXTRA="$(OPENSSL_CFLAGS_EXTRA)" \
	    "$(CURDIR)/scripts/build-openssl.sh" \
	        --cc "$(STATIC_CC)" \
	        --prefix "$(abspath $(OPENSSL_PREFIX))" \
	        --work-dir "$(abspath $(OPENSSL_WORK))" \
	        --version "$(OPENSSL_VERSION)"; \
	    printf '%s\n' "$$state" > "$(OPENSSL_PREFIX)/.vendor-state"; \
	fi

# Fully static build against musl libc (or MinGW). Cross compilation works the
# same way:
#   CC=aarch64-linux-musl-gcc make static
#   CC=x86_64-w64-mingw32-gcc make static
static: $(VENDOR_DEP)
	@mkdir -p "$(BUILD_DIR)"
	@if [ "$(VENDOR)" = "0" ] && [ -z "$(STATIC_OPENSSL_ROOT)" ]; then \
	    printf '#include <openssl/ssl.h>\nint main(void){return 0;}\n' | \
	        $(STATIC_CC) -E -xc - >/dev/null 2>&1 || { \
	            echo "error: no OpenSSL headers available for $(STATIC_CC)" >&2; \
	            echo "       run 'make static' to vendor OpenSSL, or set OPENSSL_ROOT_DIR" >&2; \
	            exit 1; }; \
	fi
	@$(STATIC_PKG_CONFIG) CC="$(STATIC_CC)" $(CMAKE) -S . -B "$(BUILD_DIR)" \
	    -DCMAKE_BUILD_TYPE=Release \
	    $(STATIC_PLATFORM) \
	    -DCMAKE_C_FLAGS="-Os -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables" \
	    -DCMAKE_EXE_LINKER_FLAGS="$(STATIC_LDFLAGS)" \
	    -U '*OPENSSL*' \
	    $(STATIC_OPENSSL_CMAKE)
	@$(CMAKE) --build "$(BUILD_DIR)" --parallel
	@file "$(BUILD_DIR)/ttyd-client$(STATIC_EXE)"

install: build
	@$(CMAKE) --install "$(BUILD_DIR)"

uninstall:
	@rm -f "$(PREFIX)/bin/ttyd-client" "$(PREFIX)/share/man/man1/ttyd-client.1"

format:
	@clang-format -i $(SRC) $(HDR)

clean:
	@rm -rf "$(BUILD_DIR)"

distclean: clean
	@rm -rf build build-musl build-musl-tls build-notls ttyd-client
