#pragma once

#include <cstdint>

namespace snp::gles3 {

// F3DEX2 Display List Opcodes
constexpr uint8_t G_NOOP = 0x00;
constexpr uint8_t G_VTX = 0x01;
constexpr uint8_t G_MODIFYVTX = 0x02;
constexpr uint8_t G_CULLDL = 0x03;
constexpr uint8_t G_BRANCH_Z = 0x04;
constexpr uint8_t G_TRI1 = 0x05;
constexpr uint8_t G_TRI2 = 0x06;
constexpr uint8_t G_QUAD = 0x07;
constexpr uint8_t G_SPECIAL_3 = 0xD3;
constexpr uint8_t G_SPECIAL_2 = 0xD4;
constexpr uint8_t G_SPECIAL_1 = 0xD5;
constexpr uint8_t G_DMA_IO = 0xD6;
constexpr uint8_t G_TEXTURE = 0xD7;
constexpr uint8_t G_POPMTX = 0xD8;
constexpr uint8_t G_GEOMETRYMODE = 0xD9;
constexpr uint8_t G_MTX = 0xDA;
constexpr uint8_t G_MOVEWORD = 0xDB;
constexpr uint8_t G_MOVEMEM = 0xDC;
constexpr uint8_t G_LOAD_UCODE = 0xDD;
constexpr uint8_t G_DL = 0xDE;
constexpr uint8_t G_ENDDL = 0xDF;
constexpr uint8_t G_SPNOOP = 0xE0;
constexpr uint8_t G_RDPHALF_1 = 0xE1;
constexpr uint8_t G_SETOTHERMODE_L = 0xE2;
constexpr uint8_t G_SETOTHERMODE_H = 0xE3;
constexpr uint8_t G_TEXRECT = 0xE4;
constexpr uint8_t G_TEXRECTFLIP = 0xE5;
constexpr uint8_t G_RDPLOADSYNC = 0xE6;
constexpr uint8_t G_RDPPIPESYNC = 0xE7;
constexpr uint8_t G_RDPTILESYNC = 0xE8;
constexpr uint8_t G_RDPFULLSYNC = 0xE9;
constexpr uint8_t G_SETKEYGB = 0xEA;
constexpr uint8_t G_SETKEYR = 0xEB;
constexpr uint8_t G_SETCONVERT = 0xEC;
constexpr uint8_t G_SETSCISSOR = 0xED;
constexpr uint8_t G_SETPRIMDEPTH = 0xEE;
constexpr uint8_t G_RDPSETOTHERMODE = 0xEF;
constexpr uint8_t G_LOADTLUT = 0xF0;
constexpr uint8_t G_RDPHALF_2 = 0xF1;
constexpr uint8_t G_SETTILESIZE = 0xF2;
constexpr uint8_t G_LOADBLOCK = 0xF3;
constexpr uint8_t G_LOADTILE = 0xF4;
constexpr uint8_t G_SETTILE = 0xF5;
constexpr uint8_t G_FILLRECT = 0xF6;
constexpr uint8_t G_SETFILLCOLOR = 0xF7;
constexpr uint8_t G_SETFOGCOLOR = 0xF8;
constexpr uint8_t G_SETBLENDCOLOR = 0xF9;
constexpr uint8_t G_SETPRIMCOLOR = 0xFA;
constexpr uint8_t G_SETENVCOLOR = 0xFB;
constexpr uint8_t G_SETCOMBINE = 0xFC;
constexpr uint8_t G_SETTIMG = 0xFD;
constexpr uint8_t G_SETZIMG = 0xFE;
constexpr uint8_t G_SETCIMG = 0xFF;

// RT64 Extended Opcodes
constexpr uint8_t G_EX = 0x64;
constexpr uint32_t G_EX_SETSCISSOR = 0x05;
constexpr uint32_t G_EX_SETRECTALIGN = 0x06;
constexpr uint32_t G_EX_MATRIXGROUP = 0x0C;
constexpr uint32_t G_EX_POPMATRIXGROUP = 0x0D;
constexpr uint32_t G_EX_PUSHSCISSOR = 0x17;
constexpr uint32_t G_EX_POPSCISSOR = 0x18;
constexpr uint32_t G_EX_SETRDRAMEXTENDED = 0x2C;
constexpr uint32_t G_EX_ENABLE_MAGIC = 0x00525464u;

// HUD Rect Alignment Origin
constexpr uint32_t G_EX_ORIGIN_LEFT = 0x000u;
constexpr uint32_t G_EX_ORIGIN_RIGHT = 0x400u;
constexpr uint32_t G_EX_ORIGIN_NONE = 0x800u;
constexpr uint32_t G_EX_ORIGIN_TOP = 0x000u;
constexpr uint32_t G_EX_ORIGIN_BOTTOM = 0x400u;

// Geometry Mode Flags
constexpr uint32_t G_ZBUFFER = 0x00000001u;
constexpr uint32_t G_SHADE = 0x00000004u;
constexpr uint32_t G_CULL_FRONT = 0x00000200u;
constexpr uint32_t G_CULL_BACK = 0x00000400u;
constexpr uint32_t G_FOG = 0x00010000u;
constexpr uint32_t G_LIGHTING = 0x00020000u;
constexpr uint32_t G_TEXTURE_GEN = 0x00040000u;
constexpr uint32_t G_TEXTURE_GEN_LINEAR = 0x00080000u;
constexpr uint32_t G_SHADING_SMOOTH = 0x00200000u;
constexpr uint32_t G_CLIPPING = 0x00800000u;

// Matrix Command Flags
// In F3DEX2, parameter byte in w0 has G_MTX_PUSH inverted by the gSPMatrix macro:
// params = (w0 & 0xFF) ^ 1:
// bit 0: push (1 = push, 0 = nopush)
// bit 1: load vs mul (1 = load, 0 = mul)
// bit 2: projection vs modelview (1 = projection, 0 = modelview)
constexpr uint8_t G_MTX_MODELVIEW = 0x00;
constexpr uint8_t G_MTX_PROJECTION = 0x04;
constexpr uint8_t G_MTX_MUL = 0x00;
constexpr uint8_t G_MTX_LOAD = 0x02;
constexpr uint8_t G_MTX_NOPUSH = 0x00;
constexpr uint8_t G_MTX_PUSH = 0x01;

// Image Formats
constexpr uint8_t G_IM_FMT_RGBA = 0;
constexpr uint8_t G_IM_FMT_YUV = 1;
constexpr uint8_t G_IM_FMT_CI = 2;
constexpr uint8_t G_IM_FMT_IA = 3;
constexpr uint8_t G_IM_FMT_I = 4;

// Image Bit Sizes
constexpr uint8_t G_IM_SIZ_4b = 0;
constexpr uint8_t G_IM_SIZ_8b = 1;
constexpr uint8_t G_IM_SIZ_16b = 2;
constexpr uint8_t G_IM_SIZ_32b = 3;

// Cycle Types
constexpr uint32_t G_CYC_1CYCLE = 0;
constexpr uint32_t G_CYC_2CYCLE = 1;
constexpr uint32_t G_CYC_COPY = 2;
constexpr uint32_t G_CYC_FILL = 3;

// Texture LUT Modes
constexpr uint32_t G_TT_NONE = 0;
constexpr uint32_t G_TT_RGBA16 = 2;
constexpr uint32_t G_TT_IA16 = 3;

// Texture Filter Modes
constexpr uint32_t G_TF_POINT = 0;
constexpr uint32_t G_TF_BILERP = 2;
constexpr uint32_t G_TF_AVERAGE = 3;

// Alpha Compare Modes
constexpr uint32_t G_AC_NONE = 0;
constexpr uint32_t G_AC_THRESHOLD = 1;
constexpr uint32_t G_AC_DITHER = 3;

// Other Mode L Flags
constexpr uint32_t Z_CMP = 0x0010u;
constexpr uint32_t Z_UPD = 0x0020u;
constexpr uint32_t ZMODE_DEC = 0x0800u;
constexpr uint32_t ZMODE_XLU = 0x0400u;
constexpr uint32_t CVG_X_ALPHA = 0x1000u;
constexpr uint32_t ALPHA_CVG_SEL = 0x2000u;
constexpr uint32_t FORCE_BL = 0x4000u;

// MoveMem Indices
constexpr uint8_t G_MV_VIEWPORT = 0x08;
constexpr uint8_t G_MV_LIGHT = 0x0A;

// MoveWord Indices
constexpr uint8_t G_MW_MATRIX = 0x00;
constexpr uint8_t G_MW_NUMLIGHT = 0x02;
constexpr uint8_t G_MW_CLIP = 0x04;
constexpr uint8_t G_MW_SEGMENT = 0x06;
constexpr uint8_t G_MW_FOG = 0x08;
constexpr uint8_t G_MW_LIGHTCOL = 0x0A;

// Combiner Mux Constants
constexpr uint32_t G_CCMUX_COMBINED = 0;
constexpr uint32_t G_CCMUX_TEXEL0 = 1;
constexpr uint32_t G_CCMUX_TEXEL1 = 2;
constexpr uint32_t G_CCMUX_PRIMITIVE = 3;
constexpr uint32_t G_CCMUX_SHADE = 4;
constexpr uint32_t G_CCMUX_ENVIRONMENT = 5;
constexpr uint32_t G_CCMUX_1 = 6;
constexpr uint32_t G_CCMUX_NOISE = 7;
constexpr uint32_t G_CCMUX_0 = 31;

constexpr uint32_t G_CCMUX_COMBINED_ALPHA = 7;
constexpr uint32_t G_CCMUX_TEXEL0_ALPHA = 8;
constexpr uint32_t G_CCMUX_TEXEL1_ALPHA = 9;
constexpr uint32_t G_CCMUX_PRIMITIVE_ALPHA = 10;
constexpr uint32_t G_CCMUX_SHADE_ALPHA = 11;
constexpr uint32_t G_CCMUX_ENV_ALPHA = 12;
constexpr uint32_t G_CCMUX_LOD_FRACTION = 13;
constexpr uint32_t G_CCMUX_PRIM_LOD_FRAC = 14;

constexpr uint32_t G_ACMUX_COMBINED = 0;
constexpr uint32_t G_ACMUX_TEXEL0 = 1;
constexpr uint32_t G_ACMUX_TEXEL1 = 2;
constexpr uint32_t G_ACMUX_PRIMITIVE = 3;
constexpr uint32_t G_ACMUX_SHADE = 4;
constexpr uint32_t G_ACMUX_ENVIRONMENT = 5;
constexpr uint32_t G_ACMUX_1 = 6;
constexpr uint32_t G_ACMUX_0 = 7;

} // namespace snp::gles3
