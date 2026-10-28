#!/bin/bash
#
# Cross build ttyd-client for every platform that ttyd ships binaries for.
#
# The musl toolchains are the same ones used by the ttyd release build, so the
# resulting binaries are fully static and have no run time dependencies.
# OpenSSL is always vendored and linked statically, so wss:// works in every
# binary.
#
# Example:
#         env BUILD_TARGET=mips ./scripts/cross-build.sh
#         env BUILD_TARGET=aarch64 ./scripts/cross-build.sh
#
set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
CROSS_ROOT="${CROSS_ROOT:-/opt/cross}"
STAGE_ROOT="${STAGE_ROOT:-/opt/stage}"
BUILD_ROOT="${BUILD_ROOT:-/opt/build}"
BUILD_TARGET="${BUILD_TARGET:-x86_64}"

OPENSSL_VERSION="${OPENSSL_VERSION:-3.6.1}"
export OPENSSL_CFLAGS_EXTRA="${OPENSSL_CFLAGS_EXTRA:-}"

install_cmake_cross_file() {
    cat << EOF > "${BUILD_DIR}/cross-${TARGET}.cmake"
SET(CMAKE_SYSTEM_NAME $1)

set(CMAKE_C_COMPILER "${TARGET}-gcc")

set(CMAKE_FIND_ROOT_PATH "${STAGE_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(OPENSSL_ROOT_DIR "${STAGE_DIR}")
set(OPENSSL_USE_STATIC_LIBS TRUE)

# Do not let pkg-config look at the host packages: the host libcrypto.pc brings
# dependencies that do not exist in the target sysroot and make FindOpenSSL ask
# CMake for targets that cannot be resolved.
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR} "${STAGE_DIR}/lib/pkgconfig:${STAGE_DIR}/lib64/pkgconfig")
EOF
}

build_client() {
    echo "=== Building ttyd-client (${ALIAS})..."
    rm -rf "${BUILD_DIR}/client" && mkdir -p "${BUILD_DIR}/client" && cd "${BUILD_DIR}/client"
    cmake -DCMAKE_TOOLCHAIN_FILE="${BUILD_DIR}/cross-${TARGET}.cmake" \
        -DCMAKE_INSTALL_PREFIX="${STAGE_DIR}" \
        -DCMAKE_BUILD_TYPE=RELEASE \
        -U '*OPENSSL*' \
        -DCMAKE_C_FLAGS="-Os -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -flto" \
        -DCMAKE_EXE_LINKER_FLAGS="-static -no-pie -Wl,-s -Wl,--gc-sections" \
        "${PROJECT_DIR}"
    make -j"$(nproc)"

    mkdir -p "${PROJECT_DIR}/build"
    local output_name="ttyd-client"
    local source_name="ttyd-client"
    [ "${ALIAS}" = "win32" ] && output_name="ttyd-client.exe" && source_name="ttyd-client.exe"
    cp "${source_name}" "${PROJECT_DIR}/build/${output_name}"
    file "${PROJECT_DIR}/build/${output_name}"
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

    echo "=== Building target ${ALIAS} (${TARGET})..."
    rm -rf "${STAGE_DIR}" "${BUILD_DIR}"
    mkdir -p "${STAGE_DIR}" "${BUILD_DIR}"
    export PKG_CONFIG_PATH="${STAGE_DIR}/lib/pkgconfig:${STAGE_DIR}/lib64/pkgconfig"

    install_cmake_cross_file ${SYSTEM}

    "${SCRIPT_DIR}/build-openssl.sh" \
        --cc gcc \
        --cross-compile "${TARGET}-" \
        --triple "${TARGET}" \
        --prefix "${STAGE_DIR}" \
        --work-dir "${BUILD_DIR}/openssl" \
        --version "${OPENSSL_VERSION}"

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
