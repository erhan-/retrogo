#!/bin/sh
# Build all libretro cores for Prime Go
set -e

# Build toolchain image if not present
if ! docker images -q primego-toolchain | grep -q .; then
    echo "Building toolchain image..."
    docker build -t primego-toolchain .
fi

mkdir -p ../cores
docker run --rm -v "$(pwd)/../cores:/out" primego-toolchain sh -c '
set -e

build_core() {
    CORENAME=$1
    REPO=$2
    PLATFORM=$3
    MAKEFILE=$4
    EXTRA_FLAGS=$5
    cd /tmp && rm -rf build_core && git clone --depth 1 "$REPO" build_core 2>/dev/null
    cd build_core
    make -f "$MAKEFILE" platform="$PLATFORM" CC=arm-linux-gnueabihf-gcc CXX=arm-linux-gnueabihf-g++ $EXTRA_FLAGS -j4 2>&1 | tail -3
    arm-linux-gnueabihf-strip ${CORENAME}_libretro.so
    cp ${CORENAME}_libretro.so /out/
    echo "  OK: ${CORENAME}_libretro.so ($(ls -lh /out/${CORENAME}_libretro.so | awk "{print \$5}"))"
}

echo "=== Gearboy (Game Boy/GBC) ==="
build_core gearboy https://github.com/drhelius/Gearboy.git unix Makefile ""

echo "=== Snes9x (SNES) ==="
build_core snes9x https://github.com/libretro/snes9x.git classic_armv7_a7 Makefile ""

echo "=== PCSX-ReARMed (PSX) ==="
build_core pcsx_rearmed https://github.com/libretro/pcsx_rearmed.git unix Makefile.libretro "HAVE_NEON=0 BUILTIN_GPU=unai ARCH=arm"

echo "=== Done ==="
'

echo "Cores in ../cores/"
ls -lh ../cores/*.so 2>/dev/null