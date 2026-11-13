#!/bin/bash
set -euo pipefail

# Automated build & packaging script for Sin and Punishment (NextUI H700 / PortMaster)
# Target: AArch64 (ARM64) Cortex-A53, glibc 2.31 (Ubuntu 20.04 LTS baseline)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=== Sin and Punishment AArch64 PortMaster Build ==="
echo "Repository root: $REPO_ROOT"

cd "$REPO_ROOT"

TOOLBOX_CMD="toolbox run --container fedora-toolbox-44"

# 1. Host code generation
echo "--> Checking host tools and code generation..."
ROM_JP="$REPO_ROOT/Tsumi to Batsu - Hoshi no Keishousha (Japan).z64"

if [ ! -f "$ROM_JP" ]; then
    echo "Notice: $ROM_JP not found. Generating placeholder/existing code."
fi

# Build host tools and patches if not already built
if [ ! -f "$REPO_ROOT/RecompiledPatches/patches.c" ] || [ ! -f "$REPO_ROOT/build-host/file_to_c" ]; then
    echo "Configuring and building host tools (file_to_c, N64RecompCLI, RSPRecomp, PatchesLib)..."
    $TOOLBOX_CMD cmake -B build-host -G Ninja \
        -DSNP_BACKEND_GLES3=ON \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++
    $TOOLBOX_CMD cmake --build build-host --target file_to_c N64RecompCLI RSPRecomp PatchesLib
fi

# Locate host recompiler binaries (CMAKE_RUNTIME_OUTPUT_DIRECTORY outputs directly to build-host/)
N64RECOMP_BIN=""
if [ -x "$REPO_ROOT/build-host/N64Recomp" ]; then
    N64RECOMP_BIN="$REPO_ROOT/build-host/N64Recomp"
elif [ -x "$REPO_ROOT/build-host/N64RecompCLI" ]; then
    N64RECOMP_BIN="$REPO_ROOT/build-host/N64RecompCLI"
elif [ -x "$REPO_ROOT/build-host/lib/N64ModernRuntime/N64Recomp/N64RecompCLI" ]; then
    N64RECOMP_BIN="$REPO_ROOT/build-host/lib/N64ModernRuntime/N64Recomp/N64RecompCLI"
fi

RSPRECOMP_BIN=""
if [ -x "$REPO_ROOT/build-host/RSPRecomp" ]; then
    RSPRECOMP_BIN="$REPO_ROOT/build-host/RSPRecomp"
elif [ -x "$REPO_ROOT/build-host/lib/N64ModernRuntime/N64Recomp/RSPRecomp" ]; then
    RSPRECOMP_BIN="$REPO_ROOT/build-host/lib/N64ModernRuntime/N64Recomp/RSPRecomp"
fi

# Generate RecompiledFuncs from ROM if present
if [ -f "$ROM_JP" ] && [ -n "$N64RECOMP_BIN" ] && [ ! -f "$REPO_ROOT/RecompiledFuncs/funcs.h" ]; then
    echo "--> Running host N64Recomp on Japanese build ROM..."
    $TOOLBOX_CMD "$N64RECOMP_BIN" snp.jp.toml
fi

# Generate RSP audio microcode if present
if [ -f "$ROM_JP" ] && [ -n "$RSPRECOMP_BIN" ] && [ ! -f "$REPO_ROOT/rsp/n_aspMain.cpp" ]; then
    echo "--> Generating RSP audio microcode..."
    mkdir -p "$REPO_ROOT/rsp"
    $TOOLBOX_CMD "$RSPRECOMP_BIN" n_aspMain.jp.toml
fi

# 2. Containerized Cross-Compilation (Ubuntu 20.04 Focal for glibc 2.31 compatibility)
echo "--> Starting containerized cross-compilation (Ubuntu 20.04 glibc 2.31)..."

# SELinux guardrail: NEVER use :Z with $HOME / $REPO_ROOT; use --security-opt label=disable
podman run --rm \
    --security-opt label=disable \
    -v "$REPO_ROOT":"$REPO_ROOT" \
    -w "$REPO_ROOT" \
    docker.io/library/ubuntu:20.04 \
    bash -c '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
export TZ=Etc/UTC

# Configure multiarch apt repositories
cat << "EOF" > /etc/apt/sources.list
deb [arch=amd64] http://archive.ubuntu.com/ubuntu/ focal main restricted universe multiverse
deb [arch=amd64] http://archive.ubuntu.com/ubuntu/ focal-updates main restricted universe multiverse
deb [arch=amd64] http://security.ubuntu.com/ubuntu/ focal-security main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports/ focal main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports/ focal-updates main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports/ focal-security main restricted universe multiverse
EOF

dpkg --add-architecture arm64
apt-get update
apt-get install -y --no-install-recommends \
    ninja-build \
    gcc \
    libc6-dev \
    g++-10-aarch64-linux-gnu \
    gcc-10-aarch64-linux-gnu \
    libsdl2-dev:arm64 \
    libgles2-mesa-dev:arm64 \
    pkg-config \
    python3-pip \
    ca-certificates \
    git \
    make

# Install CMake >= 3.24 (focal apt cmake is only 3.16)
pip3 install --no-cache-dir "cmake>=3.24,<4.0"

# Compile native host file_to_c for CrossCompile.cmake import
mkdir -p build-host-focal
gcc -O2 tools/file_to_c.c -o build-host-focal/file_to_c

BUILD_DIR="build-portmaster-aarch64"
mkdir -p "$BUILD_DIR"

export PKG_CONFIG_LIBDIR="/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig"

cmake -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc-10 \
    -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++-10 \
    -DCMAKE_CROSSCOMPILING=ON \
    -DSNP_BACKEND_GLES3=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-mcpu=cortex-a53 -ffp-contract=off" \
    -DCMAKE_CXX_FLAGS="-mcpu=cortex-a53 -ffp-contract=off" \
    -DSNP_HOST_TOOLS_DIR="$PWD/build-host-focal" \
    -DCMAKE_FIND_ROOT_PATH="/usr/aarch64-linux-gnu;/usr/lib/aarch64-linux-gnu"

ninja -C "$BUILD_DIR" -j"$(nproc)" SnPRecompiled
aarch64-linux-gnu-strip "$BUILD_DIR/SnPRecompiled"
'

# 3. Package PortMaster zip
echo "--> Staging and packaging PortMaster distribution..."
mkdir -p "$REPO_ROOT/dist"
mkdir -p "$REPO_ROOT/portmaster/sinandpunishment"

cp -f "$REPO_ROOT/build-portmaster-aarch64/SnPRecompiled" "$REPO_ROOT/portmaster/sinandpunishment/SnPRecompiled"
chmod +x "$REPO_ROOT/portmaster/sinandpunishment/SnPRecompiled"
chmod +x "$REPO_ROOT/portmaster/Sin and Punishment.sh"

DIST_ZIP="$REPO_ROOT/dist/sinandpunishment-portmaster-h700.zip"
rm -f "$DIST_ZIP"

cd "$REPO_ROOT/portmaster"
# Create zip via python zipfile module to avoid host dependency on bare zip
python3 -c "
import os, zipfile
dist_zip = '$DIST_ZIP'
with zipfile.ZipFile(dist_zip, 'w', zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk('.'):
        for file in files:
            p = os.path.join(root, file)
            # Skip any ROM dumps in the staged package zip
            rel = os.path.relpath(p, '.')
            if rel.startswith('sinandpunishment/roms/') and not rel.endswith('.gitkeep'):
                continue
            z.write(p, rel)
"
cd "$REPO_ROOT"

# Update dist/rgsp-ready directory (uses NextUI PORTS.pak .ports/ layout)
echo "--> Updating dist/rgsp-ready handheld directory..."
RGSP_PORT_DIR="$REPO_ROOT/dist/rgsp-ready/.ports/sinandpunishment"
mkdir -p "$RGSP_PORT_DIR/saves"
mkdir -p "$RGSP_PORT_DIR/roms"
cp -f "$REPO_ROOT/portmaster/Sin and Punishment.sh" "$REPO_ROOT/dist/rgsp-ready/"
cp -f "$REPO_ROOT/portmaster/sinandpunishment/snprecomp.toml" "$RGSP_PORT_DIR/"
cp -f "$REPO_ROOT/portmaster/sinandpunishment/port.json" "$RGSP_PORT_DIR/"
cp -f "$REPO_ROOT/portmaster/sinandpunishment/README.md" "$RGSP_PORT_DIR/"
cp -f "$REPO_ROOT/build-portmaster-aarch64/SnPRecompiled" "$RGSP_PORT_DIR/SnPRecompiled"
touch "$RGSP_PORT_DIR/saves/.gitkeep"
touch "$RGSP_PORT_DIR/roms/.gitkeep"
for rom in "$REPO_ROOT/portmaster/sinandpunishment/roms"/*.[zZnNvV]64 \
           "$REPO_ROOT/dist/rgsp-ready/sinandpunishment/roms"/*.[zZnNvV]64; do
    if [ -f "$rom" ]; then
        cp -u "$rom" "$RGSP_PORT_DIR/roms/"
    fi
done
rm -rf "$REPO_ROOT/dist/rgsp-ready/sinandpunishment"

echo "=== Build Complete! ==="
echo "Output package: $DIST_ZIP"
