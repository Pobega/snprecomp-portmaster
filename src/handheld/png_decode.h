#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace snp::handheld {

struct DecodedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

bool decode_png(const uint8_t* data, size_t size, DecodedImage& out);

} // namespace snp::handheld
