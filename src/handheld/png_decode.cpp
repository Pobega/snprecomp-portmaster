#include "png_decode.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "miniz.h"

namespace snp::handheld {

namespace {

constexpr uint8_t PNG_SIGNATURE[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
constexpr uint32_t MAX_PNG_DIMENSION = 4096;

constexpr uint8_t PNG_COLOR_GRAY = 0;
constexpr uint8_t PNG_COLOR_RGB = 2;
constexpr uint8_t PNG_COLOR_INDEXED = 3;
constexpr uint8_t PNG_COLOR_GRAY_ALPHA = 4;
constexpr uint8_t PNG_COLOR_RGBA = 6;

constexpr uint8_t PNG_FILTER_NONE = 0;
constexpr uint8_t PNG_FILTER_SUB = 1;
constexpr uint8_t PNG_FILTER_UP = 2;
constexpr uint8_t PNG_FILTER_AVERAGE = 3;
constexpr uint8_t PNG_FILTER_PAETH = 4;

uint32_t read_big_endian_u32(const uint8_t* ptr) {
    return (static_cast<uint32_t>(ptr[0]) << 24) |
           (static_cast<uint32_t>(ptr[1]) << 16) |
           (static_cast<uint32_t>(ptr[2]) << 8) |
           static_cast<uint32_t>(ptr[3]);
}

uint8_t paeth_predictor(int a, int b, int c) {
    int p = a + b - c;
    int pa = std::abs(p - a);
    int pb = std::abs(p - b);
    int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) {
        return static_cast<uint8_t>(a);
    }
    if (pb <= pc) {
        return static_cast<uint8_t>(b);
    }
    return static_cast<uint8_t>(c);
}

int bytes_per_pixel(uint8_t color_type) {
    switch (color_type) {
    case PNG_COLOR_GRAY:
        return 1;
    case PNG_COLOR_RGB:
        return 3;
    case PNG_COLOR_INDEXED:
        return 1;
    case PNG_COLOR_GRAY_ALPHA:
        return 2;
    case PNG_COLOR_RGBA:
        return 4;
    default:
        return 0;
    }
}

} // namespace

bool decode_png(const uint8_t* data, size_t size, DecodedImage& out) {
    if (data == nullptr || size < sizeof(PNG_SIGNATURE) + 12) {
        return false;
    }

    if (std::memcmp(data, PNG_SIGNATURE, sizeof(PNG_SIGNATURE)) != 0) {
        return false;
    }

    size_t cursor = sizeof(PNG_SIGNATURE);
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t bit_depth = 0;
    uint8_t color_type = 0;
    bool has_ihdr = false;

    std::vector<uint8_t> palette;
    std::vector<uint8_t> trns;
    std::vector<uint8_t> compressed_idat;

    while (cursor + 8 <= size) {
        uint32_t chunk_len = read_big_endian_u32(data + cursor);
        uint32_t chunk_type = read_big_endian_u32(data + cursor + 4);
        cursor += 8;

        if (cursor + chunk_len + 4 > size) {
            return false;
        }

        const uint8_t* chunk_data = data + cursor;

        // "IHDR"
        if (chunk_type == 0x49484452) {
            if (chunk_len < 13) {
                return false;
            }
            width = read_big_endian_u32(chunk_data);
            height = read_big_endian_u32(chunk_data + 4);
            bit_depth = chunk_data[8];
            color_type = chunk_data[9];
            uint8_t compression = chunk_data[10];
            uint8_t filter = chunk_data[11];
            uint8_t interlace = chunk_data[12];

            if (width == 0 || width > MAX_PNG_DIMENSION || height == 0 || height > MAX_PNG_DIMENSION) {
                return false;
            }
            if (bit_depth != 8 || compression != 0 || filter != 0 || interlace != 0) {
                return false;
            }
            if (bytes_per_pixel(color_type) == 0) {
                return false;
            }
            has_ihdr = true;
        }
        // "PLTE"
        else if (chunk_type == 0x504C5445) {
            if (chunk_len % 3 != 0 || chunk_len > 256 * 3) {
                return false;
            }
            palette.assign(chunk_data, chunk_data + chunk_len);
        }
        // "tRNS"
        else if (chunk_type == 0x74524E53) {
            trns.assign(chunk_data, chunk_data + chunk_len);
        }
        // "IDAT"
        else if (chunk_type == 0x49444154) {
            compressed_idat.insert(compressed_idat.end(), chunk_data, chunk_data + chunk_len);
        }
        // "IEND"
        else if (chunk_type == 0x49454E44) {
            break;
        }

        cursor += chunk_len + 4; // skip data and 4-byte CRC
    }

    if (!has_ihdr || compressed_idat.empty()) {
        return false;
    }

    if (color_type == PNG_COLOR_INDEXED && palette.empty()) {
        return false;
    }

    int bpp = bytes_per_pixel(color_type);
    size_t line_payload_bytes = static_cast<size_t>(width) * bpp;
    size_t scanline_stride = 1 + line_payload_bytes;
    size_t total_uncompressed_bytes = static_cast<size_t>(height) * scanline_stride;

    std::vector<uint8_t> uncompressed(total_uncompressed_bytes);
    mz_ulong dest_len = static_cast<mz_ulong>(total_uncompressed_bytes);
    int status = mz_uncompress(uncompressed.data(), &dest_len,
                               compressed_idat.data(), static_cast<mz_ulong>(compressed_idat.size()));
    if (status != MZ_OK || dest_len != total_uncompressed_bytes) {
        return false;
    }

    out.width = width;
    out.height = height;
    out.rgba.resize(static_cast<size_t>(width) * height * 4);

    std::vector<uint8_t> current_line(line_payload_bytes);
    std::vector<uint8_t> prior_line(line_payload_bytes, 0);

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* src_scanline = uncompressed.data() + (y * scanline_stride);
        uint8_t filter_type = src_scanline[0];
        const uint8_t* raw = src_scanline + 1;

        switch (filter_type) {
        case PNG_FILTER_NONE:
            std::memcpy(current_line.data(), raw, line_payload_bytes);
            break;
        case PNG_FILTER_SUB:
            for (size_t i = 0; i < line_payload_bytes; ++i) {
                uint8_t a = (i >= static_cast<size_t>(bpp)) ? current_line[i - bpp] : 0;
                current_line[i] = static_cast<uint8_t>(raw[i] + a);
            }
            break;
        case PNG_FILTER_UP:
            for (size_t i = 0; i < line_payload_bytes; ++i) {
                current_line[i] = static_cast<uint8_t>(raw[i] + prior_line[i]);
            }
            break;
        case PNG_FILTER_AVERAGE:
            for (size_t i = 0; i < line_payload_bytes; ++i) {
                int a = (i >= static_cast<size_t>(bpp)) ? current_line[i - bpp] : 0;
                int b = prior_line[i];
                current_line[i] = static_cast<uint8_t>(raw[i] + ((a + b) / 2));
            }
            break;
        case PNG_FILTER_PAETH:
            for (size_t i = 0; i < line_payload_bytes; ++i) {
                int a = (i >= static_cast<size_t>(bpp)) ? current_line[i - bpp] : 0;
                int b = prior_line[i];
                int c = (i >= static_cast<size_t>(bpp)) ? prior_line[i - bpp] : 0;
                current_line[i] = static_cast<uint8_t>(raw[i] + paeth_predictor(a, b, c));
            }
            break;
        default:
            return false;
        }

        uint8_t* dst_row = out.rgba.data() + (static_cast<size_t>(y) * width * 4);

        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* dst_px = dst_row + (x * 4);

            if (color_type == PNG_COLOR_RGBA) {
                const uint8_t* p = current_line.data() + (x * 4);
                dst_px[0] = p[0];
                dst_px[1] = p[1];
                dst_px[2] = p[2];
                dst_px[3] = p[3];
            } else if (color_type == PNG_COLOR_RGB) {
                const uint8_t* p = current_line.data() + (x * 3);
                dst_px[0] = p[0];
                dst_px[1] = p[1];
                dst_px[2] = p[2];
                dst_px[3] = 255;
            } else if (color_type == PNG_COLOR_INDEXED) {
                uint8_t idx = current_line[x];
                if (static_cast<size_t>(idx) * 3 + 2 >= palette.size()) {
                    return false;
                }
                dst_px[0] = palette[idx * 3 + 0];
                dst_px[1] = palette[idx * 3 + 1];
                dst_px[2] = palette[idx * 3 + 2];
                dst_px[3] = (idx < trns.size()) ? trns[idx] : 255;
            } else if (color_type == PNG_COLOR_GRAY_ALPHA) {
                const uint8_t* p = current_line.data() + (x * 2);
                dst_px[0] = p[0];
                dst_px[1] = p[0];
                dst_px[2] = p[0];
                dst_px[3] = p[1];
            } else if (color_type == PNG_COLOR_GRAY) {
                uint8_t gray = current_line[x];
                dst_px[0] = gray;
                dst_px[1] = gray;
                dst_px[2] = gray;
                dst_px[3] = 255;
            }
        }

        prior_line = current_line;
    }

    return true;
}

} // namespace snp::handheld
