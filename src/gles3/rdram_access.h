#pragma once

#include <cstdint>
#include <cstring>

namespace snp::gles3 {

constexpr uint32_t RDRAM_SIZE = 0x00800000u;
constexpr uint32_t RDRAM_PHYSICAL_MASK = 0x007FFFFFu;

inline uint32_t physical_rdram_offset(uint32_t addr) {
    return addr & RDRAM_PHYSICAL_MASK;
}

inline uint32_t read_rdram_u32(const uint8_t* rdram, uint32_t phys_addr) {
    uint32_t val = 0;
    std::memcpy(&val, rdram + (phys_addr & 0x007FFFFCu), sizeof(uint32_t));
    return val;
}

inline uint16_t read_rdram_u16(const uint8_t* rdram, uint32_t phys_addr) {
    uint16_t val = 0;
    std::memcpy(&val, rdram + ((phys_addr ^ 2u) & 0x007FFFFEu), sizeof(uint16_t));
    return val;
}

inline int16_t read_rdram_s16(const uint8_t* rdram, uint32_t phys_addr) {
    return static_cast<int16_t>(read_rdram_u16(rdram, phys_addr));
}

inline uint8_t read_rdram_u8(const uint8_t* rdram, uint32_t phys_addr) {
    return rdram[(phys_addr ^ 3u) & RDRAM_PHYSICAL_MASK];
}

inline int8_t read_rdram_s8(const uint8_t* rdram, uint32_t phys_addr) {
    return static_cast<int8_t>(read_rdram_u8(rdram, phys_addr));
}

inline void write_rdram_u32(uint8_t* rdram, uint32_t phys_addr, uint32_t val) {
    std::memcpy(rdram + (phys_addr & 0x007FFFFCu), &val, sizeof(uint32_t));
}

inline void write_rdram_u16(uint8_t* rdram, uint32_t phys_addr, uint16_t val) {
    std::memcpy(rdram + ((phys_addr ^ 2u) & 0x007FFFFEu), &val, sizeof(uint16_t));
}

inline void write_rdram_s16(uint8_t* rdram, uint32_t phys_addr, int16_t val) {
    write_rdram_u16(rdram, phys_addr, static_cast<uint16_t>(val));
}

inline void write_rdram_u8(uint8_t* rdram, uint32_t phys_addr, uint8_t val) {
    rdram[(phys_addr ^ 3u) & RDRAM_PHYSICAL_MASK] = val;
}

inline uint32_t resolve_segment_address(uint32_t segmented_addr, const uint32_t segments[16]) {
    if (segmented_addr & 0x80000000u) {
        return segmented_addr & RDRAM_PHYSICAL_MASK;
    }
    const uint32_t seg_index = (segmented_addr >> 24) & 0x0Fu;
    return (segments[seg_index] + (segmented_addr & 0x00FFFFFFu)) & RDRAM_PHYSICAL_MASK;
}

} // namespace snp::gles3
