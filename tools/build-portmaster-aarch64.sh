#!/bin/bash
set -euo pipefail

# Automated build & packaging script for Sin and Punishment (PortMaster)
# Target: AArch64 (ARM64) Cortex-A53, glibc 2.31 (Ubuntu 20.04 LTS baseline)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=== Sin and Punishment AArch64 PortMaster Build ==="
echo "Repository root: $REPO_ROOT"

cd "$REPO_ROOT"

if [ -f /run/.containerenv ]; then
    TOOLBOX_CMD=""
else
    TOOLBOX_CMD="toolbox run --container fedora-toolbox-44"
fi

if command -v podman >/dev/null 2>&1; then
    PODMAN_CMD="podman"
elif command -v flatpak-spawn >/dev/null 2>&1; then
    PODMAN_CMD="flatpak-spawn --host podman"
else
    PODMAN_CMD="podman"
fi

# 1. Host code generation
echo "--> Checking host tools and code generation..."
ROM_JP="$REPO_ROOT/Tsumi to Batsu - Hoshi no Keishousha (Japan).z64"

if [ ! -f "$ROM_JP" ]; then
    echo "Notice: $ROM_JP not found. Generating placeholder/existing code."
fi

# Build host tools and patches if not already built
if [ ! -f "$REPO_ROOT/RecompiledPatches/patches.c" ] || [ ! -f "$REPO_ROOT/build-host/file_to_c" ] || [ ! -f "$REPO_ROOT/build-host/snp-patch" ]; then
    echo "Configuring and building host tools (file_to_c, N64RecompCLI, RSPRecomp, PatchesLib, snp-patch)..."
    $TOOLBOX_CMD cmake -B build-host -G Ninja \
        -DSNP_BACKEND_GLES3=ON \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++
    $TOOLBOX_CMD cmake --build build-host --target file_to_c N64RecompCLI RSPRecomp PatchesLib snp-patch
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
ROM_EN="$REPO_ROOT/snp.n64.en.fan.z64"
RECOMP_NEEDED=0
if [ ! -f "$REPO_ROOT/RecompiledFuncs/funcs.h" ]; then
    RECOMP_NEEDED=1
elif [ -f "$REPO_ROOT/snp.jp.toml" ] && [ "$REPO_ROOT/snp.jp.toml" -nt "$REPO_ROOT/RecompiledFuncs/funcs.h" ]; then
    RECOMP_NEEDED=1
fi

if [ "$RECOMP_NEEDED" = "1" ] && [ -n "$N64RECOMP_BIN" ]; then
    CLEANUP_ROM_JP=0
    if [ ! -f "$ROM_JP" ] && [ -f "$ROM_EN" ] && [ -f "$REPO_ROOT/tools/prepare_rom_for_recomp.py" ]; then
        echo "--> Generating Japanese ROM from English ROM for recompiler..."
        python3 "$REPO_ROOT/tools/prepare_rom_for_recomp.py" "$ROM_EN"
        CLEANUP_ROM_JP=1
    fi

    if [ -f "$ROM_JP" ]; then
        echo "--> Running host N64Recomp on Japanese build ROM..."
        $TOOLBOX_CMD "$N64RECOMP_BIN" snp.jp.toml
        touch "$REPO_ROOT/RecompiledFuncs/funcs.h"
        if [ "$CLEANUP_ROM_JP" = "1" ]; then
            rm -f "$ROM_JP"
        fi
    fi
fi

# Generate RSP audio microcode if present
if [ -f "$ROM_JP" ] && [ -n "$RSPRECOMP_BIN" ] && [ ! -f "$REPO_ROOT/rsp/n_aspMain.cpp" ]; then
    echo "--> Generating RSP audio microcode..."
    mkdir -p "$REPO_ROOT/rsp"
    $TOOLBOX_CMD "$RSPRECOMP_BIN" n_aspMain.jp.toml
fi

# 2. Containerized Cross-Compilation (Ubuntu 20.04 Focal for glibc 2.31 compatibility)
PACKAGE_ONLY="${PACKAGE_ONLY:-0}"
if [ "$PACKAGE_ONLY" = "1" ] && [ -f "$REPO_ROOT/build-portmaster-aarch64/SnPRecompiled" ] && [ -f "$REPO_ROOT/build-portmaster-aarch64/snp-patch" ]; then
    echo "--> PACKAGE_ONLY=1 set and binaries exist; skipping cross-compilation."
else
    echo "--> Checking/building cached container toolchain image..."
    IMAGE_TAG="localhost/snprecomp-focal-arm64:latest"
    if ! $PODMAN_CMD image exists "$IMAGE_TAG" || ! $PODMAN_CMD run --rm "$IMAGE_TAG" which g++ >/dev/null 2>&1; then
        echo "Building/updating $IMAGE_TAG..."
        $PODMAN_CMD build -t "$IMAGE_TAG" - << "EOF"
FROM docker.io/library/ubuntu:20.04
ENV DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC
RUN cat << "SOURCES" > /etc/apt/sources.list
deb [arch=amd64] http://archive.ubuntu.com/ubuntu/ focal main restricted universe multiverse
deb [arch=amd64] http://archive.ubuntu.com/ubuntu/ focal-updates main restricted universe multiverse
deb [arch=amd64] http://security.ubuntu.com/ubuntu/ focal-security main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports/ focal main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports/ focal-updates main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports/ focal-security main restricted universe multiverse
SOURCES
RUN dpkg --add-architecture arm64 && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        ninja-build \
        gcc \
        g++ \
        libc6-dev \
        g++-10-aarch64-linux-gnu \
        gcc-10-aarch64-linux-gnu \
        libsdl2-dev:arm64 \
        libgles2-mesa-dev:arm64 \
        pkg-config \
        python3-pip \
        ca-certificates \
        git \
        make && \
    pip3 install --no-cache-dir "cmake>=3.24,<4.0" && \
    rm -rf /var/lib/apt/lists/*
EOF
    fi

    echo "--> Running cross-compilation in $IMAGE_TAG..."
    # SELinux guardrail: NEVER use :Z with $HOME / $REPO_ROOT; use --security-opt label=disable
    $PODMAN_CMD run --rm \
        --security-opt label=disable \
        -v "$REPO_ROOT":"$REPO_ROOT" \
        -w "$REPO_ROOT" \
        "$IMAGE_TAG" \
        bash -c '
set -euo pipefail
mkdir -p build-host-focal
if [ ! -x build-host-focal/file_to_c ]; then
    gcc -O2 tools/file_to_c.c -o build-host-focal/file_to_c
fi
g++ -O2 -std=c++17 tools/snp_patch.cpp -Itools -o build-host-focal/snp-patch

BUILD_DIR="build-portmaster-aarch64"
mkdir -p "$BUILD_DIR"
export PKG_CONFIG_LIBDIR="/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig"

if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    cmake -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
        -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc-10 \
        -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++-10 \
        -DCMAKE_CROSSCOMPILING=ON \
        -DSNP_BACKEND_GLES3=ON \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="-march=armv8-a -ffp-contract=off" \
        -DCMAKE_CXX_FLAGS="-march=armv8-a -ffp-contract=off" \
        -DSNP_HOST_TOOLS_DIR="$PWD/build-host-focal" \
        -DCMAKE_FIND_ROOT_PATH="/usr/aarch64-linux-gnu;/usr/lib/aarch64-linux-gnu"
fi

ninja -C "$BUILD_DIR" -j"$(nproc)" SnPRecompiled snp-patch
aarch64-linux-gnu-strip "$BUILD_DIR/SnPRecompiled" "$BUILD_DIR/snp-patch"
'
fi

# 3. Generate upstream patch
echo "--> Generating PortMaster GLES3 patch..."
mkdir -p "$REPO_ROOT/portmaster/patches"
git add -N src/ tests/ tools/snp_patch.cpp tools/snp_patch_lib.h 2>/dev/null || true
git diff 6e76c24c84e6fc65faeae89e8e75d7fac92aba1f -- \
    CMakeLists.txt include/ lib-patches/ src/ tests/ snp.jp.toml \
    tools/file_to_c.c tools/file_to_c.py tools/prepare_rom_for_recomp.py \
    tools/snp_patch.cpp tools/snp_patch_lib.h \
    > "$REPO_ROOT/portmaster/patches/snprecomp-portmaster-gles3.patch"

# 4. Locate host snp-patch and valid build ROM for keystream encryption
echo "--> Locating valid build ROM for keystream encryption..."
SNP_PATCH_HOST=""
if [ -x "$REPO_ROOT/build-host-focal/snp-patch" ]; then
    SNP_PATCH_HOST="$REPO_ROOT/build-host-focal/snp-patch"
elif [ -x "$REPO_ROOT/build-host/snp-patch" ]; then
    SNP_PATCH_HOST="$REPO_ROOT/build-host/snp-patch"
else
    mkdir -p "$REPO_ROOT/build-host-focal"
    $TOOLBOX_CMD g++ -O2 -std=c++17 "$REPO_ROOT/tools/snp_patch.cpp" -I"$REPO_ROOT/tools" -o "$REPO_ROOT/build-host-focal/snp-patch"
    SNP_PATCH_HOST="$REPO_ROOT/build-host-focal/snp-patch"
fi

KEY_ROM=""
if [ -n "${SNP_BUILD_ROM:-}" ] && [ -f "$SNP_BUILD_ROM" ]; then
    if "$SNP_PATCH_HOST" --verify "$SNP_BUILD_ROM" >/dev/null 2>&1; then
        KEY_ROM="$SNP_BUILD_ROM"
    fi
fi

if [ -z "$KEY_ROM" ]; then
    shopt -s nullglob nocaseglob
    rom_candidates=("$REPO_ROOT/"*.{z64,n64,v64} "$REPO_ROOT/roms/"*.{z64,n64,v64})
    shopt -u nullglob nocaseglob
    for cand in "${rom_candidates[@]}"; do
        if "$SNP_PATCH_HOST" --verify "$cand" >/dev/null 2>&1; then
            KEY_ROM="$cand"
            break
        fi
    done
fi

if [ -z "$KEY_ROM" ]; then
    echo "Error: No valid Sin and Punishment Nintendo 64 ROM found to generate snp.patch."
    echo "Place an unaltered Japanese or verified English Sin and Punishment ROM in $REPO_ROOT or set SNP_BUILD_ROM."
    exit 1
fi
echo "Using build ROM for keystream encryption: $(basename "$KEY_ROM")"

# 5. Compress and encrypt snp.patch
echo "--> Generating clean-room ROM-keyed snp.patch..."
mkdir -p "$REPO_ROOT/portmaster/sinandpunishment"
gzip -9 -n -c "$REPO_ROOT/build-portmaster-aarch64/SnPRecompiled" > "$REPO_ROOT/build-portmaster-aarch64/SnPRecompiled.gz"
"$SNP_PATCH_HOST" --crypt "$KEY_ROM" "$REPO_ROOT/build-portmaster-aarch64/SnPRecompiled.gz" "$REPO_ROOT/portmaster/sinandpunishment/snp.patch"
rm -f "$REPO_ROOT/build-portmaster-aarch64/SnPRecompiled.gz"

mkdir -p "$REPO_ROOT/portmaster/sinandpunishment/tools"
cp -f "$REPO_ROOT/build-portmaster-aarch64/snp-patch" "$REPO_ROOT/portmaster/sinandpunishment/tools/snp-patch"
chmod +x "$REPO_ROOT/portmaster/sinandpunishment/tools/snp-patch"
chmod +x "$REPO_ROOT/portmaster/sinandpunishment/tools/patchscript"
chmod +x "$REPO_ROOT/portmaster/Sin and Punishment.sh"
rm -f "$REPO_ROOT/portmaster/sinandpunishment/snprecompiled.aarch64" "$REPO_ROOT/portmaster/sinandpunishment/SnPRecompiled"

# 6. Assemble the 2 clean distribution outputs
echo "--> Assembling distribution outputs in dist/..."
mkdir -p "$REPO_ROOT/dist"

# Output 1: PR Submission Layout (dist/portmaster-pr/sinandpunishment/)
echo "--> Assembling dist/portmaster-pr/sinandpunishment/..."
PR_DIR="$REPO_ROOT/dist/portmaster-pr/sinandpunishment"
rm -rf "$REPO_ROOT/dist/portmaster-pr"
mkdir -p "$PR_DIR/patches"
mkdir -p "$PR_DIR/sinandpunishment/licenses"
mkdir -p "$PR_DIR/sinandpunishment/roms"
mkdir -p "$PR_DIR/sinandpunishment/saves"
mkdir -p "$PR_DIR/sinandpunishment/tools"

cp -f "$REPO_ROOT/portmaster/port.json" "$PR_DIR/"
cp -f "$REPO_ROOT/portmaster/README.md" "$PR_DIR/"
cp -f "$REPO_ROOT/portmaster/gameinfo.xml" "$PR_DIR/"
cp -f "$REPO_ROOT/portmaster/screenshot.png" "$PR_DIR/"
cp -f "$REPO_ROOT/portmaster/Sin and Punishment.sh" "$PR_DIR/"
cp -f "$REPO_ROOT/portmaster/patches/snprecomp-portmaster-gles3.patch" "$PR_DIR/patches/"

cp -f "$REPO_ROOT/portmaster/sinandpunishment/snp.patch" "$PR_DIR/sinandpunishment/"
cp -f "$REPO_ROOT/portmaster/sinandpunishment/snprecomp.toml" "$PR_DIR/sinandpunishment/"
cp -f "$REPO_ROOT/portmaster/sinandpunishment/tools/snp-patch" "$PR_DIR/sinandpunishment/tools/"
cp -f "$REPO_ROOT/portmaster/sinandpunishment/tools/patchscript" "$PR_DIR/sinandpunishment/tools/"
chmod +x "$PR_DIR/sinandpunishment/tools/snp-patch" "$PR_DIR/sinandpunishment/tools/patchscript" "$PR_DIR/Sin and Punishment.sh"
cp -rf "$REPO_ROOT/portmaster/sinandpunishment/licenses/"* "$PR_DIR/sinandpunishment/licenses/"
touch "$PR_DIR/sinandpunishment/roms/.gitkeep"
touch "$PR_DIR/sinandpunishment/saves/.gitkeep"

# Output 2: PortMaster Testing Zip (dist/sinandpunishment.zip)
echo "--> Packaging dist/sinandpunishment.zip..."
DIST_ZIP="$REPO_ROOT/dist/sinandpunishment.zip"
rm -f "$DIST_ZIP"

python3 -c "
import os, zipfile

def add_entry(z, src, arcname, compress=zipfile.ZIP_DEFLATED):
    is_exec = arcname.endswith('.sh') or arcname.endswith('patchscript') or os.path.basename(arcname) == 'snp-patch'
    mode = 0o100755 if is_exec else 0o100644
    zinfo = zipfile.ZipInfo.from_file(src, arcname)
    zinfo.external_attr = mode << 16
    with open(src, 'rb') as f:
        z.writestr(zinfo, f.read(), compress_type=compress)

dist_zip = '$DIST_ZIP'
with zipfile.ZipFile(dist_zip, 'w', zipfile.ZIP_DEFLATED) as z:
    add_entry(z, '$REPO_ROOT/portmaster/Sin and Punishment.sh', 'Sin and Punishment.sh')
    for bf in ['port.json', 'README.md', 'gameinfo.xml', 'screenshot.png', 'testing_thread.txt']:
        p = os.path.join('$REPO_ROOT/portmaster', bf)
        if os.path.exists(p):
            add_entry(z, p, os.path.join('sinandpunishment', bf))
    add_entry(z, '$REPO_ROOT/portmaster/patches/snprecomp-portmaster-gles3.patch', 'sinandpunishment/patches/snprecomp-portmaster-gles3.patch')
    add_entry(z, '$REPO_ROOT/portmaster/sinandpunishment/snp.patch', 'sinandpunishment/snp.patch', compress=zipfile.ZIP_STORED)
    add_entry(z, '$REPO_ROOT/portmaster/sinandpunishment/tools/patchscript', 'sinandpunishment/tools/patchscript')
    add_entry(z, '$REPO_ROOT/portmaster/sinandpunishment/tools/snp-patch', 'sinandpunishment/tools/snp-patch')
    add_entry(z, '$REPO_ROOT/portmaster/sinandpunishment/snprecomp.toml', 'sinandpunishment/snprecomp.toml')
    lic_dir = '$REPO_ROOT/portmaster/sinandpunishment/licenses'
    for lf in sorted(os.listdir(lic_dir)):
        lp = os.path.join(lic_dir, lf)
        if os.path.isfile(lp):
            add_entry(z, lp, os.path.join('sinandpunishment/licenses', lf))
    for d in ['roms', 'saves']:
        gk = os.path.join('$REPO_ROOT/portmaster/sinandpunishment', d, '.gitkeep')
        if not os.path.exists(gk):
            with open(gk, 'w') as f: pass
        add_entry(z, gk, os.path.join('sinandpunishment', d, '.gitkeep'))
"

echo "=== Build Complete! ==="
echo "PR directory:   $PR_DIR"
echo "Testing zip:    $DIST_ZIP"
