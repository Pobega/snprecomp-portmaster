#!/usr/bin/env python3
"""
Prepares a Sin and Punishment ROM for recompilation and handheld packaging.

Accepts any .z64, .n64, or .v64 dump of either the Japanese release or an English
fan translation. Normalizes endianness to big-endian .z64, validates the code
fingerprint against the port's target, and if an English fan translation is supplied,
reconstructs a build-only Japanese ROM ('Tsumi to Batsu - Hoshi no Keishousha (Japan).z64')
so N64Recomp and RSPRecomp generate 100% matching code.

Optionally stages the ROM into the PortMaster distribution directory.
"""

import argparse
import hashlib
import os
import shutil
import struct
import sys

Z64_MAGIC = bytes.fromhex("80371240")
V64_MAGIC = bytes.fromhex("37804012")
N64_MAGIC = bytes.fromhex("40123780")

MAIN_VRAM = 0x80025C00
MAIN_ROM = 0x1000
MAIN_TEXT_SIZE = 0x31280
LOAD_TABLE_ROM = 0x34BAC

CODE_FINGERPRINT_JP = 0xe941bdc06663d9ad

SHA1_JP = "581297b9d5c3a4c33169ae0aae218c742cd9cbcf"
SHA1_EN_FAN = "5945ff3935d9d9531bbb5cfcabed7c198e2b2f41"

VARYING_WORDS = [
    0x80025C00, 0x80025C04, 0x80025C08, 0x80025C0C,
    0x8002BD5C, 0x8002BD8C, 0x8002BD98,
    0x8003AB28, 0x8003AB2C, 0x8003AB78, 0x8003AB7C, 0x8003ABA4, 0x8003ABA8, 0x8003ABF0, 0x8003ABF4,
    0x8003C158, 0x8003C15C, 0x8003C198, 0x8003C19C, 0x8003C1EC, 0x8003C1F0
]

# Japanese instructions to restore in English ROMs for N64Recomp (snp.jp.toml lines 234-285)
JP_INSTRUCTIONS = [
    # func_8002BCE0 display list address constants
    (0x8002BD5C, 0x24425D20),
    (0x8002BD8C, 0x24425D20),
    (0x8002BD98, 0x24425D20),
    # func_8003C0D0 asset table ROM offset constants
    (0x8003C158, 0x3C0200A9),
    (0x8003C15C, 0x2443CA40),
    (0x8003C198, 0x3C0200A9),
    (0x8003C19C, 0x2442CA40),
    (0x8003C1EC, 0x3C0200A9),
    (0x8003C1F0, 0x2443CA40),
    # func_8003A9CC wave table ROM offset constants
    (0x8003AB28, 0x3C02013C),
    (0x8003AB2C, 0x24442C20),
    (0x8003AB78, 0x3C02013C),
    (0x8003AB7C, 0x24432C20),
    (0x8003ABA4, 0x3C02013C),
    (0x8003ABA8, 0x24422C20),
    (0x8003ABF0, 0x3C02013C),
    (0x8003ABF4, 0x24432C20)
]

# Standard Japanese entry point IPL3 boot code (0x80025C00..0x80025C0C)
JP_ENTRY_INSTRUCTIONS = [
    (0x80025C00, 0x3C088006),
    (0x80025C04, 0x25083450),
    (0x80025C08, 0x3C090004),
    (0x80025C0C, 0x25293C20)
]


def normalize_endianness(data: bytes) -> tuple[bytearray, str]:
    magic = data[0:4]
    out = bytearray(data)
    if magic == Z64_MAGIC:
        return out, "z64 (big-endian native)"
    if magic == V64_MAGIC:
        for i in range(0, len(out), 2):
            out[i], out[i + 1] = out[i + 1], out[i]
        return out, "v64 (byte-swapped 16-bit) -> normalized"
    if magic == N64_MAGIC:
        for i in range(0, len(out), 4):
            out[i:i + 4] = out[i:i + 4][::-1]
        return out, "n64 (little-endian 32-bit) -> normalized"
    raise ValueError(f"Unrecognized ROM magic bytes: {magic.hex()}")


def fnv1a_64(data: bytes, h: int) -> int:
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def compute_code_fingerprint(rom: bytes) -> int:
    if len(rom) < MAIN_ROM + MAIN_TEXT_SIZE or len(rom) < LOAD_TABLE_ROM + 20:
        return 0
    main = bytearray(rom[MAIN_ROM:MAIN_ROM + MAIN_TEXT_SIZE])
    for vram in VARYING_WORDS:
        offset = vram - MAIN_VRAM
        if offset + 4 <= len(main):
            main[offset:offset + 4] = bytes(4)

    h = fnv1a_64(main, 0xcbf29ce484222325)
    p = LOAD_TABLE_ROM
    while p + 20 <= len(rom):
        entry = rom[p:p + 20]
        rom_start, vram, text_end, _, _ = struct.unpack(">5I", entry)
        h = fnv1a_64(entry, h)
        if rom_start == 0 or vram == 0:
            return h
        if text_end < vram or rom_start + (text_end - vram) > len(rom):
            return 0
        h = fnv1a_64(rom[rom_start:rom_start + (text_end - vram)], h)
        p += 20
    return h


def reconstruct_build_rom(normalized: bytearray) -> bytearray:
    build_rom = bytearray(normalized)

    # 1. Restore standard entrypoint IPL3 boot code if modified by PALadin / patching stub
    for vram, word in JP_ENTRY_INSTRUCTIONS:
        offset = MAIN_ROM + (vram - MAIN_VRAM)
        struct.pack_into(">I", build_rom, offset, word)

    # 2. Restore Japanese instructions for N64Recomp
    for vram, word in JP_INSTRUCTIONS:
        offset = MAIN_ROM + (vram - MAIN_VRAM)
        struct.pack_into(">I", build_rom, offset, word)

    # 3. Restore Japanese cartridge header
    # Internal name: 'TSUMI TO BATSU      ' (20 bytes at 0x20)
    jp_name = b"TSUMI TO BATSU      "
    build_rom[0x20:0x34] = jp_name

    # Game code: 'NGUJ' at 0x3C
    build_rom[0x3C:0x40] = b"NGUJ"

    # CRCs at 0x10 and 0x14
    struct.pack_into(">I", build_rom, 0x10, 0xB6BC0FB0)
    struct.pack_into(">I", build_rom, 0x14, 0xE3812198)

    return build_rom


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Prepare Sin and Punishment ROM for recompilation and handheld packaging."
    )
    parser.add_argument("input_rom", help="Path to input .z64, .n64, or .v64 ROM file")
    parser.add_argument(
        "-o", "--output",
        default="Tsumi to Batsu - Hoshi no Keishousha (Japan).z64",
        help="Output path for build-ready Japanese ROM (default: 'Tsumi to Batsu - Hoshi no Keishousha (Japan).z64')"
    )
    parser.add_argument(
        "--stage-portmaster",
        action="store_true",
        help="Stage the input ROM into portmaster/sinandpunishment/roms/ directory"
    )
    parser.add_argument(
        "--portmaster-dir",
        default="portmaster/sinandpunishment/roms",
        help="Destination directory when --stage-portmaster is used"
    )

    args = parser.parse_args()

    if not os.path.isfile(args.input_rom):
        sys.exit(f"Error: input ROM file not found: {args.input_rom}")

    with open(args.input_rom, "rb") as f:
        raw_data = f.read()

    normalized, format_note = normalize_endianness(raw_data)
    sha1 = hashlib.sha1(normalized).hexdigest()
    fingerprint = compute_code_fingerprint(normalized)

    print(f"Format: {format_note}")
    print(f"SHA-1 : {sha1}")
    print(f"Code Fingerprint: 0x{fingerprint:016x}")

    if fingerprint != CODE_FINGERPRINT_JP:
        sys.exit(f"Error: ROM code fingerprint (0x{fingerprint:016x}) does not match Sin and Punishment (0x{CODE_FINGERPRINT_JP:016x})!")

    if sha1 == SHA1_JP:
        print("Detected original Japanese release ROM.")
        with open(args.output, "wb") as f:
            f.write(normalized)
        print(f"Wrote build ROM -> {args.output}")
    else:
        print("Detected fan English translation or alternate dump with matching code.")
        print("Reconstructing build-ready Japanese code segment for N64Recomp...")
        build_rom = reconstruct_build_rom(normalized)
        with open(args.output, "wb") as f:
            f.write(build_rom)
        print(f"Wrote build ROM -> {args.output}")

    if args.stage_portmaster:
        os.makedirs(args.portmaster_dir, exist_ok=True)
        dest_filename = os.path.basename(args.input_rom)
        if not dest_filename.lower().endswith(".z64"):
            dest_filename = os.path.splitext(dest_filename)[0] + ".z64"
        dest_path = os.path.join(args.portmaster_dir, dest_filename)
        with open(dest_path, "wb") as f:
            f.write(normalized)
        print(f"Staged runtime ROM for PortMaster -> {dest_path}")


if __name__ == "__main__":
    main()
