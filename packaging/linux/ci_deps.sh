#!/usr/bin/env bash
# Build dependencies for the Linux release on Ubuntu 24.04 (glibc 2.39: the AppImage then runs on
# Ubuntu 24.04+, Fedora 40+, SteamOS and the like). Used by .github/workflows/linux-release.yml;
# also works in an Ubuntu 24.04 container (distrobox create -i ubuntu:24.04).
#   bash packaging/linux/ci_deps.sh apt      the distribution packages (sudo)
#   bash packaging/linux/ci_deps.sh source   what Ubuntu 24.04 has too old, built into $DEPS_PREFIX
#   source packaging/linux/ci_deps.sh env    the variables build.sh needs to find them
# From source: SDL3 (Ubuntu has none), Vulkan-Headers (vk_recorder_cmds.inc is generated from
# 1.4.357), VulkanMemoryAllocator (vmaGetAllocationInfo2 is 3.1+, Ubuntu has 3.0), robin-map, and
# Zydis + Zycore as static libraries with -fPIC (they are linked into libbbgpu.so).
DEPS_PREFIX=${DEPS_PREFIX:-$HOME/sotc-deps}
SDL_VERSION=release-3.2.24
VULKAN_HEADERS_VERSION=v1.4.357
VMA_VERSION=v3.3.0
ROBIN_MAP_VERSION=v1.4.1
ZYDIS_VERSION=v4.1.1

deps_env() {
    local p=$DEPS_PREFIX
    export CMAKE_PREFIX_PATH="$p${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
    export PKG_CONFIG_PATH="$p/lib/pkgconfig:$p/lib64/pkgconfig:$p/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export CPATH="$p/include${CPATH:+:$CPATH}"
    export LIBRARY_PATH="$p/lib:$p/lib64${LIBRARY_PATH:+:$LIBRARY_PATH}"
    # ldd in build_appimage.sh finds libSDL3 here, and bundles it.
    export LD_LIBRARY_PATH="$p/lib:$p/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export CC=${CC:-gcc-14} CXX=${CXX:-g++-14}
}

install_apt() {
    sudo apt-get update -qq
    # Compilers and tools, the port's own libraries, then the headers SDL3 needs for its X11,
    # Wayland, audio and gamepad (udev, dbus) backends: SDL loads those libraries at run time.
    sudo apt-get install -y --no-install-recommends \
        build-essential gcc-14 g++-14 cmake ninja-build pkg-config python3 python3-tk git nasm \
        patchelf file curl ca-certificates libvulkan-dev glslang-tools libfmt-dev libboost-dev \
        libxxhash-dev \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev \
        libxfixes-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev \
        libegl-dev libgl-dev libgbm-dev libdrm-dev libpulse-dev libasound2-dev \
        libpipewire-0.3-dev libudev-dev libdbus-1-dev
}

cmake_install() {  # $1 = git url, $2 = tag, the rest = cmake options
    local url=$1 tag=$2 name work
    shift 2
    name=$(basename "$url" .git)
    work=$(mktemp -d)
    git -c advice.detachedHead=false clone --quiet --depth 1 --recurse-submodules --shallow-submodules -b "$tag" "$url" "$work/src"
    cmake -S "$work/src" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON "$@" >/dev/null
    cmake --build "$work/build" >/dev/null
    cmake --install "$work/build" >/dev/null
    echo "$name $tag -> $DEPS_PREFIX"
    # Zydis keeps Zycore as a submodule and does not install it: callers may need that tree.
    if [[ $name == zydis ]]; then
        cmake -S "$work/src/dependencies/zycore" -B "$work/zycore" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON >/dev/null
        cmake --build "$work/zycore" >/dev/null
        cmake --install "$work/zycore" >/dev/null
        echo "zycore (from zydis $tag) -> $DEPS_PREFIX"
    fi
    rm -rf "$work"
}

install_source() {
    deps_env
    mkdir -p "$DEPS_PREFIX"
    cmake_install https://github.com/libsdl-org/SDL.git "$SDL_VERSION" \
        -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_STATIC=OFF
    cmake_install https://github.com/KhronosGroup/Vulkan-Headers.git "$VULKAN_HEADERS_VERSION"
    cmake_install https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git "$VMA_VERSION"
    cmake_install https://github.com/Tessil/robin-map.git "$ROBIN_MAP_VERSION"
    cmake_install https://github.com/zyantific/zydis.git "$ZYDIS_VERSION" \
        -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_EXAMPLES=OFF -DZYDIS_BUILD_SHARED_LIB=OFF
}

# Sourced ("env"): only the variables, without changing the caller's shell options.
if [[ ${BASH_SOURCE[0]} != "$0" ]]; then
    if [[ ${1:-} == env ]]; then deps_env; else echo "usage: source ${BASH_SOURCE[0]} env" >&2; fi
    return 0
fi
set -euo pipefail
case ${1:-} in
apt) install_apt ;;
source) install_source ;;
*) echo "usage: $0 apt | source, or: source $0 env" >&2; exit 2 ;;
esac
