#!/bin/bash
# Build script for VNC client dependencies
# Cross-compiles LibVNCClient, zlib, and libjpeg-turbo for ARM

set -e  # Exit on error

# Configuration
CROSS_COMPILE=${CROSS_COMPILE:-arm-linux-gnueabihf-}
DEPS_DIR="$(pwd)/deps"
BUILD_DIR="$(pwd)/deps/build"
SRC_DIR="$(pwd)/deps/src"

# Versions
LIBVNC_VERSION="0.9.14"
ZLIB_VERSION="1.2.13"
JPEG_VERSION="2.1.5.1"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}VNC Client Dependencies Build Script${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo "Cross-compiler: ${CROSS_COMPILE}gcc"
echo "Install prefix: ${DEPS_DIR}"
echo ""

# Create directories
mkdir -p "${DEPS_DIR}"/{lib,include}
mkdir -p "${BUILD_DIR}"
mkdir -p "${SRC_DIR}"

# sha256 of each tarball this script builds. A cached or downloaded file that differs is
# refused: the source offered with a card image is the source that was compiled.
pinned_sha256() {
    case "$1" in
        zlib-1.2.13.tar.gz)             echo "b3a24de97a8fdbc835b9833169501030b8977031bcb54b3b3ac13740f846ab30" ;;
        libjpeg-turbo-2.1.5.1.tar.gz)   echo "2fdc3feb6e9deb17adec9bafa3321419aa19f8f4e5dea7bf8486844ca22207bf" ;;
        LibVNCServer-0.9.14.tar.gz)     echo "83104e4f7e28b02f8bf6b010d69b626fae591f887e949816305daebae527c9a5" ;;
        *) return 1 ;;
    esac
}

# Function to download and extract
download_and_extract() {
    local name=$1
    local url=$2
    local filename=$(basename "$url")
    
    echo -e "${YELLOW}Downloading ${name}...${NC}"
    cd "${SRC_DIR}"
    
    if [ ! -f "${filename}" ]; then
        wget -q --show-progress "${url}" || {
            echo -e "${RED}Failed to download ${name}${NC}"
            return 1
        }
    else
        echo "Already downloaded: ${filename}"
    fi
    
    local want
    want="$(pinned_sha256 "${filename}")" || {
        echo -e "${RED}No pinned sha256 for ${filename}${NC}"
        return 1
    }
    echo "${want}  ${filename}" | sha256sum -c - >/dev/null || {
        echo -e "${RED}${filename} does not match its pinned sha256${NC}"
        return 1
    }

    echo "Extracting ${filename}..."
    tar -xf "${filename}"
}

# Build zlib
build_zlib() {
    echo -e "${GREEN}Building zlib...${NC}"
    
    download_and_extract "zlib" \
        "https://github.com/madler/zlib/releases/download/v${ZLIB_VERSION}/zlib-${ZLIB_VERSION}.tar.gz"
    
    cd "${SRC_DIR}/zlib-${ZLIB_VERSION}"
    
    CC="${CROSS_COMPILE}gcc" \
    AR="${CROSS_COMPILE}ar" \
    RANLIB="${CROSS_COMPILE}ranlib" \
    ./configure --prefix="${DEPS_DIR}" --static
    
    make -j$(nproc)
    make install
    
    echo -e "${GREEN}zlib build complete${NC}"
}

# Build libjpeg-turbo
build_libjpeg() {
    echo -e "${GREEN}Building libjpeg-turbo...${NC}"
    
    download_and_extract "libjpeg-turbo" \
        "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/${JPEG_VERSION}/libjpeg-turbo-${JPEG_VERSION}.tar.gz"
    
    cd "${BUILD_DIR}"
    rm -rf libjpeg-build
    mkdir -p libjpeg-build
    cd libjpeg-build
    
    cmake "${SRC_DIR}/libjpeg-turbo-${JPEG_VERSION}" \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR=arm \
        -DCMAKE_C_COMPILER="${CROSS_COMPILE}gcc" \
        -DCMAKE_INSTALL_PREFIX="${DEPS_DIR}" \
        -DENABLE_SHARED=OFF \
        -DENABLE_STATIC=ON
    
    make -j$(nproc)
    make install
    
    echo -e "${GREEN}libjpeg-turbo build complete${NC}"
}

# Build LibVNCClient
build_libvnc() {
    echo -e "${GREEN}Building LibVNCClient...${NC}"
    
    download_and_extract "LibVNCServer" \
        "https://github.com/LibVNC/libvncserver/archive/refs/tags/LibVNCServer-${LIBVNC_VERSION}.tar.gz"
    
    cd "${BUILD_DIR}"
    rm -rf libvnc-build
    mkdir -p libvnc-build
    cd libvnc-build
    
    cmake "${SRC_DIR}/libvncserver-LibVNCServer-${LIBVNC_VERSION}" \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR=arm \
        -DCMAKE_C_COMPILER="${CROSS_COMPILE}gcc" \
        -DCMAKE_INSTALL_PREFIX="${DEPS_DIR}" \
        -DZLIB_INCLUDE_DIR="${DEPS_DIR}/include" \
        -DZLIB_LIBRARY="${DEPS_DIR}/lib/libz.a" \
        -DJPEG_INCLUDE_DIR="${DEPS_DIR}/include" \
        -DJPEG_LIBRARY="${DEPS_DIR}/lib/libjpeg.a" \
        -DBUILD_SHARED_LIBS=OFF \
        -DWITH_OPENSSL=OFF \
        -DWITH_GNUTLS=OFF \
        -DWITH_GCRYPT=OFF \
        -DWITH_PNG=OFF \
        -DWITH_SDL=OFF \
        -DWITH_GTK=OFF \
        -DWITH_FFMPEG=OFF \
        -DWITH_TIGHTVNC_FILETRANSFER=OFF
    
    make -j$(nproc)
    make install
    
    echo -e "${GREEN}LibVNCClient build complete${NC}"
}

# Main build process
main() {
    # Check for required tools
    echo "Checking for required tools..."
    for tool in wget tar cmake make ${CROSS_COMPILE}gcc; do
        if ! command -v "$tool" &> /dev/null; then
            echo -e "${RED}Error: $tool not found${NC}"
            echo "Install every host prerequisite with setup-build-env.sh, at the repo root."
            exit 1
        fi
    done
    
    echo -e "${GREEN}All required tools found${NC}"
    echo ""
    
    # Build dependencies in order
    build_zlib
    echo ""
    
    build_libjpeg
    echo ""
    
    build_libvnc
    echo ""
    
    # Summary
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}Build Complete!${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
    echo "Libraries installed to: ${DEPS_DIR}/lib"
    echo "Headers installed to: ${DEPS_DIR}/include"
    echo ""
    ls -lh "${DEPS_DIR}/lib"/*.a
    echo ""
    echo -e "${GREEN}You can now run 'make' to build the VNC client${NC}"
}

# Run main function
main
