#!/bin/bash
#
# Build a static OpenSSL for a musl or MinGW target.
#
# This is shared by the Makefile ("make static") and scripts/cross-build.sh so
# that every build path vendors OpenSSL the same way.
#
# Example:
#   scripts/build-openssl.sh --cc musl-gcc --prefix build-musl/openssl \
#                            --work-dir build-musl/openssl-build
#   scripts/build-openssl.sh --cc gcc --cross-compile aarch64-linux-musl- \
#                            --triple aarch64-linux-musl --prefix /opt/stage/aarch64
#
set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

CC_CMD=""
TRIPLE=""
PREFIX=""
WORK_DIR=""
CROSS_COMPILE=""
OPENSSL_VERSION="${OPENSSL_VERSION:-3.6.1}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 1)}"

usage() {
    echo "usage: $0 --cc <compiler> --prefix <dir> [options]" >&2
    echo "  --triple <triple>       target triple (default: <cc> -dumpmachine)" >&2
    echo "  --work-dir <dir>        download and build directory" >&2
    echo "  --cross-compile <pfx>   CROSS_COMPILE prefix passed to OpenSSL" >&2
    echo "  --version <version>     OpenSSL version (default: ${OPENSSL_VERSION})" >&2
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --cc) CC_CMD="$2"; shift 2 ;;
        --triple) TRIPLE="$2"; shift 2 ;;
        --prefix) PREFIX="$2"; shift 2 ;;
        --work-dir) WORK_DIR="$2"; shift 2 ;;
        --cross-compile) CROSS_COMPILE="$2"; shift 2 ;;
        --version) OPENSSL_VERSION="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "unknown argument: $1" >&2; usage ;;
    esac
done

[ -n "${CC_CMD}" ] || usage
[ -n "${PREFIX}" ] || usage

if [ -z "${TRIPLE}" ]; then
    TRIPLE="$(${CC_CMD} -dumpmachine 2>/dev/null || true)"
    if [ -z "${TRIPLE}" ]; then
        echo "error: cannot determine target triple from '${CC_CMD}', use --triple" >&2
        exit 1
    fi
fi

[ -n "${WORK_DIR}" ] || WORK_DIR="$(pwd)/openssl-build"
mkdir -p "$(dirname "${PREFIX}")"
PREFIX="$(cd "$(dirname "${PREFIX}")" && pwd)/$(basename "${PREFIX}")"
WORK_DIR="$(mkdir -p "${WORK_DIR}" && cd "${WORK_DIR}" && pwd)"

map_openssl_target() {
    case "$1" in
        # MinGW has to come before the plain x86 patterns.
        x86_64*mingw32*) echo mingw64 ;;
        i?86*mingw32*) echo mingw ;;
        x86_64*) echo linux-x86_64 ;;
        aarch64*|arm64*) echo linux-aarch64 ;;
        armv7*|arm-*) echo linux-armv4 ;;
        i?86*) echo linux-generic32 ;;
        mips64*) echo linux64-mips64 ;;
        mips*) echo linux-mips32 ;;
        powerpc64le*|ppc64le*) echo linux-ppc64le ;;
        powerpc64*|ppc64*) echo linux-ppc64 ;;
        s390x*) echo linux64-s390x ;;
        *) echo "" ;;
    esac
}

OPENSSL_TARGET="${OPENSSL_TARGET:-$(map_openssl_target "${TRIPLE}")}"
if [ -z "${OPENSSL_TARGET}" ]; then
    echo "error: unsupported target triple '${TRIPLE}'" >&2
    echo "       set OPENSSL_TARGET to the matching OpenSSL Configure target" >&2
    exit 1
fi

is_windows=false
case "${TRIPLE}" in
    *mingw32*) is_windows=true ;;
esac

# OpenSSL's build needs the Linux UAPI headers. Ubuntu's musl-gcc wrapper does
# not ship them, so fall back to the host copies when the target sysroot has
# none. Cross toolchains carry their own and are left alone.
OPENSSL_CFLAGS="-fPIC"
if [ "${is_windows}" = false ] && [ "$(basename "${CC_CMD}")" = "musl-gcc" ]; then
    if ! printf '#include <linux/mman.h>\n' | ${CC_CMD} -E -xc - >/dev/null 2>&1; then
        MULTIARCH="$(${CC_CMD} -dumpmachine 2>/dev/null || echo x86_64-linux-gnu)"
        OPENSSL_CFLAGS="${OPENSSL_CFLAGS} -idirafter /usr/include"
        if [ -d "/usr/include/${MULTIARCH}" ]; then
            OPENSSL_CFLAGS="${OPENSSL_CFLAGS} -idirafter /usr/include/${MULTIARCH}"
        fi
    fi
fi
if [ -n "${OPENSSL_CFLAGS_EXTRA:-}" ]; then
    OPENSSL_CFLAGS="${OPENSSL_CFLAGS} ${OPENSSL_CFLAGS_EXTRA}"
fi

# OpenSSL prepends CROSS_COMPILE to the tool names (CC, AR, RANLIB, RC, ...).
# Derive it from a prefixed compiler such as x86_64-w64-mingw32-gcc or
# aarch64-linux-musl-gcc, but never for the native musl-gcc wrapper.
CC_BASENAME="$(basename "${CC_CMD}")"
if [ -z "${CROSS_COMPILE}" ]; then
    case "${CC_BASENAME}" in
        musl-gcc|gcc|cc) ;;
        *-gcc)
            CROSS_COMPILE="${CC_BASENAME%gcc}"
            CC_PATH="$(command -v "${CC_CMD}" 2>/dev/null || echo "${CC_CMD}")"
            case "${CC_PATH}" in
                */*) export PATH="$(cd "$(dirname "${CC_PATH}")" && pwd):${PATH}" ;;
            esac
            CC_CMD="gcc"
            ;;
    esac
fi

ARCHIVE="${WORK_DIR}/openssl-${OPENSSL_VERSION}.tar.gz"
SOURCE_DIR="${WORK_DIR}/openssl-${OPENSSL_VERSION}"

if [ ! -f "${ARCHIVE}" ]; then
    URL="${OPENSSL_SOURCE_URL:-https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz}"
    echo "=== Downloading OpenSSL ${OPENSSL_VERSION}..."
    curl -fSsLo "${ARCHIVE}.tmp" "${URL}"
    mv "${ARCHIVE}.tmp" "${ARCHIVE}"
fi

echo "=== Extracting OpenSSL ${OPENSSL_VERSION}..."
rm -rf "${SOURCE_DIR}"
mkdir -p "${SOURCE_DIR}"
tar xzf "${ARCHIVE}" -C "${SOURCE_DIR}" --strip-components=1

if [ "${is_windows}" = true ]; then
    case "${OPENSSL_VERSION}" in
        3.5.5|3.6.1)
            echo "=== Applying MinGW SIO_UDP_NETRESET patch..."
            (cd "${SOURCE_DIR}" && patch --batch -p1 < "${SCRIPT_DIR}/openssl-mingw-sio-udp-netreset.patch")
            ;;
    esac
fi

echo "=== Configuring OpenSSL ${OPENSSL_VERSION} (${OPENSSL_TARGET})..."
CONFIGURE_ENV=(CC="${CC_CMD}" CFLAGS="${OPENSSL_CFLAGS}")
if [ -n "${CROSS_COMPILE}" ]; then
    CONFIGURE_ENV+=(CROSS_COMPILE="${CROSS_COMPILE}")
fi
(
    cd "${SOURCE_DIR}"
    env "${CONFIGURE_ENV[@]}" ./Configure "${OPENSSL_TARGET}" \
        no-shared no-tests no-ssl3 no-err no-zlib no-comp \
        -DOPENSSL_SMALL_FOOTPRINT \
        --libdir=lib \
        --prefix="${PREFIX}" --openssldir="${PREFIX}/ssl"
)

echo "=== Building OpenSSL ${OPENSSL_VERSION} (${OPENSSL_TARGET})..."
make -C "${SOURCE_DIR}" -j"${JOBS}" build_sw
make -C "${SOURCE_DIR}" install_sw

if [ ! -f "${PREFIX}/lib/libssl.a" ] || [ ! -f "${PREFIX}/lib/libcrypto.a" ]; then
    echo "error: static OpenSSL libraries were not installed into ${PREFIX}/lib" >&2
    exit 1
fi

echo "=== OpenSSL ${OPENSSL_VERSION} for ${TRIPLE} installed into ${PREFIX}"
