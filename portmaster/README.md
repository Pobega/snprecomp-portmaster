## Notes

Special thanks to [wootbeer](https://github.com/wootbeer/snprecomp) for creating `snprecomp`, [Wiseguy](https://github.com/N64Recomp/N64ModernRuntime) for `N64Recomp` and `N64ModernRuntime`, and Treasure for the original Nintendo 64 rail shooter and its tight blend of free-aimed gunplay and close-range sword parrying.

To play, copy your legally obtained Nintendo 64 cartridge dump (`.z64`, `.n64`, or `.v64`) into `ports/sinandpunishment/roms/` before launching.

First boot automatically selects `Stickless: Strafe + Shift` on stickless/single-stick handhelds and `Twin Stick` on dual-stick handhelds. You can press `Select` or `Menu` at any time during gameplay to open the Quick Settings overlay and customize control schemes, aiming curves, sensitivity, and graphics options.

## Controls

| Button | Action |
| --- | --- |
| D-Pad | Strafe left / right (Aim crosshair while holding L1/L2 in Stickless; navigate menus) |
| Left Analog | Strafe left / right (Move character in Twin Stick) |
| Right Analog | Reticle aiming (Twin Stick) |
| A | Fire / Sword slash (Confirm in menus; Lock-on in Twin Stick) |
| B | Jump / Double jump (Cancel in menus) |
| X | Cycle lock-on targets / Quick crosshair re-center (Dodge roll in Twin Stick) |
| Y | Dodge roll |
| L1 | Shift to manual aim (Stickless) / Jump (Twin Stick) / Strafe left (Bumper Strafe) |
| L2 | Shift to manual aim (Stickless) / Jump (Twin Stick) / Dodge roll (Legacy) |
| R1 | Fire / Sword slash |
| R2 | Fire / Sword slash |
| Start | Pause game |
| Select | Open / Close Quick Settings overlay |
| Select + Start | Quick save and exit |

## Compile

```bash
git clone --recursive https://github.com/wootbeer/snprecomp.git
cd snprecomp
git checkout 6e76c24c84e6fc65faeae89e8e75d7fac92aba1f
git submodule update --init --recursive

# Apply PortMaster GLES3 handheld patches
git apply /path/to/patches/snprecomp-portmaster-gles3.patch

# Build host tools and patches
cmake -B build-host -G Ninja -DSNP_BACKEND_GLES3=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-host --target file_to_c N64RecompCLI RSPRecomp PatchesLib

# Generate C code and audio microcode from Japanese ROM
./build-host/N64RecompCLI snp.jp.toml
./build-host/RSPRecomp n_aspMain.jp.toml

# Cross-compile SnPRecompiled for aarch64
cmake -B build-aarch64 -G Ninja \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
  -DCMAKE_CROSSCOMPILING=ON \
  -DSNP_BACKEND_GLES3=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="-march=armv8-a -ffp-contract=off" \
  -DCMAKE_CXX_FLAGS="-march=armv8-a -ffp-contract=off" \
  -DSNP_HOST_TOOLS_DIR="$PWD/build-host"
ninja -C build-aarch64 SnPRecompiled
```
