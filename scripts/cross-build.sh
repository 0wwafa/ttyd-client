#!/bin/bash
#
# Cross build ttyd-client for every platform that ttyd ships binaries for.
#
# The musl toolchains are the same ones used by the ttyd release build, so the
# resulting binaries are fully static and have no run time dependencies.
#
# Example:
#         env BUILD_TARGET=mips ./scripts/cross-build.sh
#         env BUILD_TARGET=aarch64 BUILD_TLS=0 ./scripts/cross-build.sh
#
set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
CROSS_ROOT="${CROSS_ROOT:-/opt/cross}"
STAGE_ROOT="${STAGE_ROOT:-/opt/stage}"
BUILD_ROOT="${BUILD_ROOT:-/opt/build}"
BUILD_TARGET="${BUILD_TARGET:-x86_64}"
# BUILD_TLS=1 cross builds a static OpenSSL so wss:// works in the binary
BUILD_TLS_REQUESTED="${BUILD_TLS:-1}"
BUILD_TLS="${BUILD_TLS_REQUESTED}"

OPENSSL_VERSION="${OPENSSL_VERSION:-3.6.1}"

build_openssl() {
    openssl_target=$(map_openssl_target "${ALIAS}")
    echo "=== Building openssl-${OPENSSL_VERSION} (${openssl_target})..."
    curl -fSsLo- "https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz" | tar xz -C "${BUILD_DIR}"
    pushd "${BUILD_DIR}/openssl-${OPENSSL_VERSION}"
        # no-zlib: the client never compresses, so dropping it keeps the static
        # library smaller and stops CMake's FindOpenSSL from asking for a zlib
        # that does not exist inside the musl sysroot.
        openssl_cflags="-fPIC"
        case ${ALIAS} in
            s390x) openssl_cflags="${openssl_cflags} -march=z10" ;;
            win32)
                # The workaround is already present in newer OpenSSL patch releases.
                case ${OPENSSL_VERSION} in
                    3.5.5|3.6.1)
                        patch --batch -p1 < "${SCRIPT_DIR}/openssl-mingw-sio-udp-netreset.patch"
                        ;;
                esac
                ;;
        esac
        if ! env CC=gcc CROSS_COMPILE="${TARGET}-" CFLAGS="${openssl_cflags}" \
            ./Configure "${openssl_target}" no-shared no-tests no-ssl3 no-err no-zlib no-comp \
            -DOPENSSL_SMALL_FOOTPRINT \
            --prefix="${STAGE_DIR}" --openssldir="${STAGE_DIR}/ssl"; then
            echo "=== openssl Configure failed (${openssl_target})"
            popd
            return 1
        fi
        if ! make -j"$(nproc)" build_sw; then
            echo "=== openssl build failed (${openssl_target})"
            popd
            return 1
        fi
        if ! make install_sw; then
            echo "=== openssl install failed (${openssl_target})"
            popd
            return 1
        fi
    popd
}

map_openssl_target() {
    case $1 in
        i686) echo linux-generic32 ;;
        x86_64) echo linux-x86_64 ;;
        arm|armhf|armv7l) echo linux-armv4 ;;
        aarch64) echo linux-aarch64 ;;
        mips|mipsel) echo linux-mips32 ;;
        mips64|mips64el) echo linux64-mips64 ;;
        ppc64) echo linux-ppc64 ;;
        ppc64le) echo linux-ppc64le ;;
        s390x) echo linux64-s390x ;;
        win32) echo mingw64 ;;
        *) echo "unknown openssl target: $1" && exit 1 ;;
    esac
}

install_cmake_cross_file() {
    cat << EOF > "${BUILD_DIR}/cross-${TARGET}.cmake"
SET(CMAKE_SYSTEM_NAME $1)

set(CMAKE_C_COMPILER "${TARGET}-gcc")

set(CMAKE_FIND_ROOT_PATH "${STAGE_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(OPENSSL_USE_STATIC_LIBS TRUE)

# Do not let pkg-config look at the host packages: the host libcrypto.pc brings
# -lz and -lzstd along, which do not exist in the musl sysroot and make
# FindOpenSSL ask CMake for a ZLIB target that cannot be resolved.
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR} "${STAGE_DIR}/lib64/pkgconfig:${STAGE_DIR}/lib/pkgconfig")
EOF
}

build_client() {
    echo "=== Building ttyd-client (${ALIAS})..."
    rm -rf "${BUILD_DIR}/client" && mkdir -p "${BUILD_DIR}/client" && cd "${BUILD_DIR}/client"
    cmake -DCMAKE_TOOLCHAIN_FILE="${BUILD_DIR}/cross-${TARGET}.cmake" \
        -DCMAKE_INSTALL_PREFIX="${STAGE_DIR}" \
        -DCMAKE_BUILD_TYPE=RELEASE \
        -DTC_WITH_TLS="${BUILD_TLS}" \
        -DCMAKE_C_FLAGS="-Os -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -flto" \
        -DCMAKE_EXE_LINKER_FLAGS="-static -no-pie -Wl,-s -Wl,--gc-sections" \
        "${PROJECT_DIR}"
    make -j"$(nproc)"

    mkdir -p "${PROJECT_DIR}/build"
    local suffix=""
    [ "${ALIAS}" = "win32" ] && suffix=".exe"
    cp ttyd-client "${PROJECT_DIR}/build/ttyd-client.${ALIAS}${suffix}"
    file "${PROJECT_DIR}/build/ttyd-client.${ALIAS}${suffix}"
}

build() {
    TARGET="$1"
    ALIAS="$2"
    STAGE_DIR="${STAGE_ROOT}/${TARGET}"
    BUILD_DIR="${BUILD_ROOT}/${TARGET}"
    MUSL_CC_URL="https://github.com/tsl0922/musl-toolchains/releases/download/2021-11-23"
    COMPONENTS="1"
    SYSTEM="Linux"

    if [ "${ALIAS}" = "win32" ]; then
        COMPONENTS=2
        SYSTEM="Windows"
    fi

    echo "=== Installing toolchain ${ALIAS} (${TARGET})..."

    # Prepend (not append) so that the musl cross toolchain wins over a host
    # installed duplicate, for example musl-gcc from the musl-tools package.
    mkdir -p "${CROSS_ROOT}" && export PATH="${CROSS_ROOT}/bin:${PATH}"
    if [ ! -x "${CROSS_ROOT}/bin/${TARGET}-gcc" ]; then
        curl -fSsLo- "${MUSL_CC_URL}/${TARGET}-cross.tgz" | tar xz -C "${CROSS_ROOT}" --strip-components=${COMPONENTS}
    fi

    BUILD_TLS="${BUILD_TLS_REQUESTED}"
    echo "=== Building target ${ALIAS} (${TARGET})..."

    rm -rf "${STAGE_DIR}" "${BUILD_DIR}"
    mkdir -p "${STAGE_DIR}" "${BUILD_DIR}"
    export PKG_CONFIG_PATH="${STAGE_DIR}/lib/pkgconfig"

    install_cmake_cross_file ${SYSTEM}

    if [ "${BUILD_TLS}" != "0" ]; then
        if ! build_openssl; then
            if [ "${ALLOW_NO_TLS:-0}" = "1" ]; then
                echo "=== WARNING: building ${ALIAS} without TLS support"
                BUILD_TLS=0
            else
                echo "=== ERROR: cannot build OpenSSL for ${ALIAS}."
                echo "===        Re-run with BUILD_TLS=0 (or ALLOW_NO_TLS=1) to build without TLS."
                exit 1
            fi
        fi
    fi
    build_client
}

case ${BUILD_TARGET} in
    amd64) BUILD_TARGET="x86_64" ;;
    arm64) BUILD_TARGET="aarch64" ;;
    armv7) BUILD_TARGET="armv7l" ;;
    ppc64) BUILD_TARGET="powerpc64" ;;
    ppc64le) BUILD_TARGET="powerpc64le" ;;
esac

case ${BUILD_TARGET} in
    i686|x86_64|aarch64|mips|mipsel|mips64|mips64el|powerpc64|powerpc64le|s390x)
        build "${BUILD_TARGET}-linux-musl" "${BUILD_TARGET}"
        ;;
    arm)
        build "${BUILD_TARGET}-linux-musleabi" "${BUILD_TARGET}"
        ;;
    armhf)
        build arm-linux-musleabihf "${BUILD_TARGET}"
        ;;
    armv7l)
        build armv7l-linux-musleabihf "${BUILD_TARGET}"
        ;;
    win32)
        build x86_64-w64-mingw32 "${BUILD_TARGET}"
        ;;
    *)
        echo "unknown cross target: ${BUILD_TARGET}" && exit 1
esac
