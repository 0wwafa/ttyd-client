# ttyd-client -- a companion command line client for ttyd
#
# The canonical build system is CMake (see CMakeLists.txt and the CI
# workflows). This Makefile is a thin convenience wrapper around it, plus a
# dependency free "bootstrap" build that only needs a C compiler.
#
# Examples:
#   make                  # configure + build with CMake (Release)
#   make cc               # build directly with $(CC), no CMake needed
#   make static           # fully static musl build (Linux, no TLS)
#   make static TLS=1     # fully static musl build with OpenSSL (VENDOR=1 to build it)
#   make install          # install into $(PREFIX)
#   make format           # clang-format the sources
#   make clean

CC ?= cc
PREFIX ?= /usr/local
BUILD_DIR ?= build
CMAKE ?= cmake
TLS ?= 1
BUILD_TYPE ?= Release

SRC := $(wildcard src/*.c)
HDR := $(wildcard src/*.h)

ifeq ($(TLS),1)
  TLS_FLAG := -DTC_WITH_TLS=ON
else
  TLS_FLAG := -DTC_WITH_TLS=OFF
endif

.PHONY: all build configure cc static clean distclean install uninstall format help

all: build

help:
	@echo "targets:"
	@echo "  all/build          configure and build with CMake (default)"
	@echo "  cc                 build with \$$(CC) without CMake"
	@echo "  static             static musl build (BUILD_DIR=build-musl)"
	@echo "  install            install into \$$(PREFIX)"
	@echo "  format             clang-format the sources"
	@echo "  clean              remove the build directory"

configure:
	@mkdir -p "$(BUILD_DIR)"
	@$(CMAKE) -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) $(TLS_FLAG) \
	    -DCMAKE_INSTALL_PREFIX="$(PREFIX)"

build: configure
	@$(CMAKE) --build "$(BUILD_DIR)" --parallel

# A plain compiler build, useful when CMake is not available
cc:
	@$(CC) -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE \
	    -Isrc $(TLS_FLAG) -o ttyd-client $(SRC) $$([ "$(TLS)" = "1" ] && echo "-lssl -lcrypto")
	@echo "built ./ttyd-client"

# Fully static build against musl libc. Cross compilation works the same way:
#   CC=aarch64-linux-musl-gcc make static
static:
	@mkdir -p "$(BUILD_DIR)"
	@CC="$${CC:-musl-gcc}" $(CMAKE) -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=Release $(TLS_FLAG) \
	    -DCMAKE_C_FLAGS="-Os -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables" \
	    -DCMAKE_EXE_LINKER_FLAGS="-static -no-pie -Wl,-s -Wl,--gc-sections"
	@$(CMAKE) --build "$(BUILD_DIR)" --parallel
	@file "$(BUILD_DIR)/ttyd-client"

install: build
	@$(CMAKE) --install "$(BUILD_DIR)"

uninstall:
	@rm -f "$(PREFIX)/bin/ttyd-client" "$(PREFIX)/share/man/man1/ttyd-client.1"

format:
	@clang-format -i $(SRC) $(HDR)

clean:
	@rm -rf "$(BUILD_DIR)"

distclean: clean
	@rm -rf build build-musl build-notls ttyd-client
