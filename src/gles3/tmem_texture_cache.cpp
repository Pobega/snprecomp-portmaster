#include "tmem_texture_cache.h"
#include "rdram_access.h"
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace snp::gles3 {

namespace {
    constexpr uint64_t FNV1A_OFFSET_BASIS = 0xcbf29ce484222325ULL;
    constexpr uint64_t FNV1A_PRIME = 0x100000001b3ULL;

    inline uint64_t fnv1a_step(uint64_t hash, uint8_t byte) {
        return (hash ^ byte) * FNV1A_PRIME;
    }

    inline uint64_t fnv1a_bytes(uint64_t hash, const void* data, size_t size) {
        const auto* ptr = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) {
            hash = fnv1a_step(hash, ptr[i]);
        }
        return hash;
    }

    uint32_t get_bytes_per_texel_numerator(uint8_t siz) {
        switch (siz) {
            case G_IM_SIZ_4b: return 1;
            case G_IM_SIZ_8b: return 1;
            case G_IM_SIZ_16b: return 2;
            case G_IM_SIZ_32b: return 4;
            default: return 2;
        }
    }
}

TmemTextureCache::TmemTextureCache() {
    std::memset(tmem_, 0, sizeof(tmem_));
}

TmemTextureCache::~TmemTextureCache() {
    reset();
}

void TmemTextureCache::reset() {
    for (auto& item : lru_list_) {
        if (item.texture_id != 0 && gl::glDeleteTextures) {
            gl::glDeleteTextures(1, &item.texture_id);
        }
    }
    lru_list_.clear();
    cache_map_.clear();
    std::memset(tmem_, 0, sizeof(tmem_));
    for (auto& tile : tiles_) {
        tile = TileDescriptor{};
    }
    timg_ = TextureImageState{};
}

void TmemTextureCache::set_timg(uint8_t fmt, uint8_t siz, uint32_t width, uint32_t phys_addr) {
    timg_.fmt = fmt;
    timg_.siz = siz;
    timg_.width = width;
    timg_.phys_addr = physical_rdram_offset(phys_addr);
}

void TmemTextureCache::set_tile(uint32_t tile_idx, uint8_t fmt, uint8_t siz, uint16_t line, uint16_t tmem,
                                uint8_t palette, bool clamp_t, bool mirror_t, uint8_t mask_t, uint8_t shift_t,
                                bool clamp_s, bool mirror_s, uint8_t mask_s, uint8_t shift_s) {
    TileDescriptor& tile = tiles_[tile_idx & 7];
    tile.fmt = fmt;
    tile.siz = siz;
    tile.line = line;
    tile.tmem = tmem;
    tile.palette = palette;
    tile.clamp_t = clamp_t;
    tile.mirror_t = mirror_t;
    tile.mask_t = mask_t;
    tile.shift_t = shift_t;
    tile.clamp_s = clamp_s;
    tile.mirror_s = mirror_s;
    tile.mask_s = mask_s;
    tile.shift_s = shift_s;
}

void TmemTextureCache::set_tile_size(uint32_t tile_idx, uint16_t sl, uint16_t tl, uint16_t sh, uint16_t th) {
    TileDescriptor& tile = tiles_[tile_idx & 7];
    tile.sl = sl;
    tile.tl = tl;
    tile.sh = sh;
    tile.th = th;
}

void TmemTextureCache::load_block(uint32_t tile_idx, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt, const uint8_t* rdram) {
    (void)uls;
    (void)ult;
    for (auto& t : tiles_) {
        t.loaded_via_tile = false;
    }
    TileDescriptor& tile = tiles_[tile_idx & 7];
    uint32_t num_texels = lrs + 1;
    uint32_t bytes_per_texel = get_bytes_per_texel_numerator(tile.siz);
    uint32_t total_bytes = (tile.siz == G_IM_SIZ_4b) ? ((num_texels + 1) / 2) : (num_texels * bytes_per_texel);
    uint32_t num_words = (total_bytes + 7) / 8;
    num_words = std::min(num_words, 512u - (tile.tmem & 0x1FFu));

    uint32_t words_per_line = (dxt > 0) ? ((1u << 11) / dxt) : 0;

    for (uint32_t i = 0; i < num_words; ++i) {
        uint32_t tmem_word = tile.tmem + i;
        if (words_per_line >= 2) {
            uint32_t line = (i * dxt) >> 11;
            uint32_t word_in_line = i % words_per_line;
            if (line & 1) {
                word_in_line ^= 1;
            }
            tmem_word = tile.tmem + line * words_per_line + word_in_line;
        }
        tmem_word &= 0x1FFu;
        uint32_t rdram_phys = timg_.phys_addr + i * 8;
        for (uint32_t b = 0; b < 8; ++b) {
            tmem_[tmem_word * 8 + b] = read_rdram_u8(rdram, rdram_phys + b);
        }
    }
}

void TmemTextureCache::load_tile(uint32_t tile_idx, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt, const uint8_t* rdram) {
    for (auto& t : tiles_) {
        t.loaded_via_tile = true;
    }
    TileDescriptor& tile = tiles_[tile_idx & 7];
    uint32_t ul_x = uls / 4;
    uint32_t ul_y = ult / 4;
    uint32_t lr_x = lrs / 4;
    uint32_t lr_y = lrt / 4;
    uint32_t width = (lr_x >= ul_x) ? (lr_x - ul_x + 1) : 1;
    uint32_t height = (lr_y >= ul_y) ? (lr_y - ul_y + 1) : 1;

    uint32_t bytes_per_texel = get_bytes_per_texel_numerator(tile.siz);
    uint32_t row_bytes = (tile.siz == G_IM_SIZ_4b) ? ((width + 1) / 2) : (width * bytes_per_texel);
    uint32_t words_per_row = (row_bytes + 7) / 8;

    for (uint32_t y = 0; y < height; ++y) {
        uint32_t tmem_row_start = tile.tmem + y * tile.line;
        if (tile.siz == G_IM_SIZ_4b && (ul_x & 1u) != 0) {
            uint32_t row_byte_offset = ((ul_y + y) * timg_.width + ul_x) / 2u;
            for (uint32_t x = 0; x < words_per_row; ++x) {
                uint32_t word_in_row = x;
                if (words_per_row >= 2 && (y & 1)) {
                    word_in_row ^= 1;
                }
                uint32_t tmem_word = (tmem_row_start + word_in_row) & 0x1FFu;
                uint32_t src_phys = timg_.phys_addr + row_byte_offset + x * 8;
                for (uint32_t b = 0; b < 8; ++b) {
                    uint8_t hi = read_rdram_u8(rdram, src_phys + b);
                    uint8_t lo = read_rdram_u8(rdram, src_phys + b + 1);
                    tmem_[tmem_word * 8 + b] = static_cast<uint8_t>(((hi & 0x0Fu) << 4) | ((lo >> 4) & 0x0Fu));
                }
            }
            continue;
        }
        uint32_t row_byte_offset = (tile.siz == G_IM_SIZ_4b)
            ? (((ul_y + y) * timg_.width + ul_x) / 2u)
            : (((ul_y + y) * timg_.width + ul_x) * bytes_per_texel);
        for (uint32_t x = 0; x < words_per_row; ++x) {
            uint32_t word_in_row = x;
            if (words_per_row >= 2 && (y & 1)) {
                word_in_row ^= 1;
            }
            uint32_t tmem_word = (tmem_row_start + word_in_row) & 0x1FFu;
            uint32_t src_phys = timg_.phys_addr + row_byte_offset + x * 8;
            for (uint32_t b = 0; b < 8; ++b) {
                tmem_[tmem_word * 8 + b] = read_rdram_u8(rdram, src_phys + b);
            }
        }
    }
}

void TmemTextureCache::load_tlut(uint32_t tile_idx, uint32_t count, const uint8_t* rdram) {
    TileDescriptor& tile = tiles_[tile_idx & 7];
    uint32_t base_word = (tile.tmem >= 256u) ? tile.tmem : 256u;
    uint32_t num_colors = count + 1;
    for (uint32_t i = 0; i < num_colors; ++i) {
        uint32_t tmem_word = base_word + i;
        if (tmem_word >= 512u) {
            break;
        }
        uint16_t color = read_rdram_u16(rdram, timg_.phys_addr + i * 2);
        uint32_t dest_byte = tmem_word * 8u;
        tmem_[dest_byte + 0] = static_cast<uint8_t>(color >> 8);
        tmem_[dest_byte + 1] = static_cast<uint8_t>(color & 0xFF);
    }
}

uint64_t TmemTextureCache::compute_hash(const TileDescriptor& tile, uint32_t width, uint32_t height, uint32_t textfilt, uint32_t textlut) const {
    uint64_t hash = FNV1A_OFFSET_BASIS;
    hash = fnv1a_step(hash, tile.fmt);
    hash = fnv1a_step(hash, tile.siz);
    hash = fnv1a_step(hash, static_cast<uint8_t>(tile.line & 0xFF));
    hash = fnv1a_step(hash, static_cast<uint8_t>(tile.tmem & 0xFF));
    hash = fnv1a_step(hash, tile.palette);
    hash = fnv1a_step(hash, tile.clamp_s ? 1 : 0);
    hash = fnv1a_step(hash, tile.mirror_s ? 1 : 0);
    hash = fnv1a_step(hash, tile.mask_s);
    hash = fnv1a_step(hash, tile.shift_s);
    hash = fnv1a_step(hash, tile.clamp_t ? 1 : 0);
    hash = fnv1a_step(hash, tile.mirror_t ? 1 : 0);
    hash = fnv1a_step(hash, tile.mask_t);
    hash = fnv1a_step(hash, tile.shift_t);
    hash = fnv1a_bytes(hash, &width, sizeof(width));
    hash = fnv1a_bytes(hash, &height, sizeof(height));
    hash = fnv1a_bytes(hash, &textfilt, sizeof(textfilt));
    hash = fnv1a_bytes(hash, &textlut, sizeof(textlut));

    size_t start_byte = (tile.tmem * 8) % TMEM_SIZE;
    size_t byte_count = std::min(static_cast<size_t>(tile.line * height * 8), TMEM_SIZE - start_byte);
    if (byte_count == 0) {
        byte_count = std::min(static_cast<size_t>(width * height * 2), TMEM_SIZE - start_byte);
    }
    hash = fnv1a_bytes(hash, tmem_ + start_byte, byte_count);

    if (tile.fmt == G_IM_FMT_CI || textlut != G_TT_NONE) {
        hash = fnv1a_bytes(hash, tmem_ + 0x800, 2048);
    }
    return hash;
}

void TmemTextureCache::decode_pixel(const TileDescriptor& tile, uint32_t x, uint32_t y, uint32_t textlut,
                                    uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const {
    if (tile.mask_s > 0 && tile.mask_s <= 10) {
        uint32_t mask_w = 1u << tile.mask_s;
        uint32_t masked_x = x & (mask_w - 1u);
        x = (tile.mirror_s && ((x >> tile.mask_s) & 1u)) ? ((mask_w - 1u) - masked_x) : masked_x;
    }
    if (tile.mask_t > 0 && tile.mask_t <= 10) {
        uint32_t mask_h = 1u << tile.mask_t;
        uint32_t masked_y = y & (mask_h - 1u);
        y = (tile.mirror_t && ((y >> tile.mask_t) & 1u)) ? ((mask_h - 1u) - masked_y) : masked_y;
    }

    uint32_t width = (tile.sh >= tile.sl) ? (((tile.sh - tile.sl) / 4) + 1) : 32;
    uint32_t words_per_row = tile.line;
    bool use_tlut = (tile.fmt == G_IM_FMT_CI) || (textlut != G_TT_NONE);

    if (tile.siz == G_IM_SIZ_16b) {
        uint32_t word_in_row = x / 4;
        if (words_per_row >= 2 && (y & 1)) {
            word_in_row ^= 1;
        }
        uint32_t tmem_word = (tile.tmem + y * tile.line + word_in_row) & 0x1FFu;
        uint32_t byte_idx = tmem_word * 8 + (x % 4) * 2;
        uint16_t val = (static_cast<uint16_t>(tmem_[byte_idx]) << 8) | tmem_[byte_idx + 1];

        if (tile.fmt == G_IM_FMT_RGBA) {
            r = ((val >> 11) & 0x1F) * 255 / 31;
            g = ((val >> 6) & 0x1F) * 255 / 31;
            b = ((val >> 1) & 0x1F) * 255 / 31;
            a = (val & 1) ? 255 : 0;
        } else if (tile.fmt == G_IM_FMT_IA) {
            r = g = b = (val >> 8) & 0xFF;
            a = val & 0xFF;
        } else if (tile.fmt == G_IM_FMT_I) {
            r = g = b = a = (val >> 8) & 0xFF;
        } else {
            r = g = b = 255;
            a = 255;
        }
    } else if (tile.siz == G_IM_SIZ_8b) {
        uint32_t word_in_row = x / 8;
        if (words_per_row >= 2 && (y & 1)) {
            word_in_row ^= 1;
        }
        uint32_t tmem_word = (tile.tmem + y * tile.line + word_in_row) & 0x1FFu;
        uint8_t val = tmem_[tmem_word * 8 + (x % 8)];

        if (use_tlut) {
            uint32_t tlut_addr = 0x800u + static_cast<uint32_t>(val) * 8u;
            uint16_t pal_color = (static_cast<uint16_t>(tmem_[tlut_addr]) << 8) | tmem_[tlut_addr + 1];
            if (textlut == G_TT_IA16) {
                r = g = b = (pal_color >> 8) & 0xFF;
                a = pal_color & 0xFF;
            } else {
                r = ((pal_color >> 11) & 0x1F) * 255 / 31;
                g = ((pal_color >> 6) & 0x1F) * 255 / 31;
                b = ((pal_color >> 1) & 0x1F) * 255 / 31;
                a = (pal_color & 1) ? 255 : 0;
            }
        } else if (tile.fmt == G_IM_FMT_IA) {
            r = g = b = ((val >> 4) & 0x0F) * 255 / 15;
            a = (val & 0x0F) * 255 / 15;
        } else if (tile.fmt == G_IM_FMT_I) {
            r = g = b = a = val;
        } else {
            r = g = b = val;
            a = 255;
        }
    } else if (tile.siz == G_IM_SIZ_4b) {
        uint32_t byte_idx = 0;
        if (words_per_row > 0 && (tile.loaded_via_tile || width >= 16u)) {
            uint32_t word_in_row = x / 16u;
            if (words_per_row >= 2 && (y & 1)) {
                word_in_row ^= 1;
            }
            uint32_t tmem_word = (tile.tmem + y * words_per_row + word_in_row) & 0x1FFu;
            byte_idx = tmem_word * 8u + ((x % 16u) / 2u);
        } else {
            uint32_t row_stride = (width + 1u) / 2u;
            byte_idx = ((tile.tmem * 8u) + y * row_stride + (x / 2u)) % TMEM_SIZE;
        }
        uint8_t byte_val = tmem_[byte_idx];
        uint8_t nibble = ((x & 1) == 0) ? ((byte_val >> 4) & 0x0F) : (byte_val & 0x0F);

        if (use_tlut) {
            uint32_t pal_entry = static_cast<uint32_t>(tile.palette) * 16u + nibble;
            uint32_t tlut_addr = 0x800u + pal_entry * 8u;
            uint16_t pal_color = (static_cast<uint16_t>(tmem_[tlut_addr]) << 8) | tmem_[tlut_addr + 1];
            if (textlut == G_TT_IA16) {
                r = g = b = (pal_color >> 8) & 0xFF;
                a = pal_color & 0xFF;
            } else {
                r = ((pal_color >> 11) & 0x1F) * 255 / 31;
                g = ((pal_color >> 6) & 0x1F) * 255 / 31;
                b = ((pal_color >> 1) & 0x1F) * 255 / 31;
                a = (pal_color & 1) ? 255 : 0;
            }
        } else if (tile.fmt == G_IM_FMT_IA) {
            r = g = b = ((nibble >> 1) & 0x07) * 255 / 7;
            a = (nibble & 1) ? 255 : 0;
        } else if (tile.fmt == G_IM_FMT_I) {
            r = g = b = a = nibble * 255 / 15;
        } else {
            r = g = b = nibble * 255 / 15;
            a = 255;
        }
    } else if (tile.siz == G_IM_SIZ_32b) {
        uint32_t word_in_row = x / 4;
        if (words_per_row >= 2 && (y & 1)) {
            word_in_row ^= 1;
        }
        uint32_t tmem_word = (tile.tmem + y * tile.line + word_in_row) & 0x1FFu;
        uint32_t rg_idx = tmem_word * 8 + (x % 4) * 2;
        uint32_t ba_idx = ((tmem_word + 256) & 0x1FFu) * 8 + (x % 4) * 2;
        r = tmem_[rg_idx];
        g = tmem_[rg_idx + 1];
        b = tmem_[ba_idx];
        a = tmem_[ba_idx + 1];
    } else {
        r = g = b = a = 255;
    }
}

CachedTexture TmemTextureCache::fetch_texture(uint32_t tile_idx, uint32_t textfilt, uint32_t textlut) {
    const TileDescriptor& tile = tiles_[tile_idx & 7];
    uint32_t width = (tile.sh >= tile.sl) ? (((tile.sh - tile.sl) / 4) + 1) : 32;
    uint32_t height = (tile.th >= tile.tl) ? (((tile.th - tile.tl) / 4) + 1) : 32;
    if (width == 0 || width > 512) {
        width = (tile.line > 0) ? (tile.line * 4) : 32;
    }
    if (height == 0 || height > 512) {
        height = 32;
    }
    if (!tile.clamp_s && tile.mask_s > 0 && tile.mask_s <= 9) {
        width = 1u << tile.mask_s;
    }
    if (!tile.clamp_t && tile.mask_t > 0 && tile.mask_t <= 9) {
        height = 1u << tile.mask_t;
    }
    width = std::clamp(width, 1u, 512u);
    height = std::clamp(height, 1u, 512u);

    uint64_t hash = compute_hash(tile, width, height, textfilt, textlut);
    auto it = cache_map_.find(hash);
    if (it != cache_map_.end()) {
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        return *lru_list_.begin();
    }

    std::vector<uint8_t> rgba(width * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t r = 0, g = 0, b = 0, a = 0;
            decode_pixel(tile, x, y, textlut, r, g, b, a);
            size_t idx = (y * width + x) * 4;
            rgba[idx + 0] = r;
            rgba[idx + 1] = g;
            rgba[idx + 2] = b;
            rgba[idx + 3] = a;
        }
    }

    GLuint tex_id = 0;
    gl::glGenTextures(1, &tex_id);
    gl::glBindTexture(GL_TEXTURE_2D, tex_id);
    gl::glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    GLint min_mag = (textfilt == G_TF_POINT) ? GL_NEAREST : GL_LINEAR;
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_mag);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, min_mag);

    GLint wrap_s = (tile.clamp_s || tile.mask_s == 0) ? GL_CLAMP_TO_EDGE : (tile.mirror_s ? GL_MIRRORED_REPEAT : GL_REPEAT);
    GLint wrap_t = (tile.clamp_t || tile.mask_t == 0) ? GL_CLAMP_TO_EDGE : (tile.mirror_t ? GL_MIRRORED_REPEAT : GL_REPEAT);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_s);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_t);

    if (lru_list_.size() >= MAX_CACHE_ENTRIES) {
        CachedTexture oldest = lru_list_.back();
        lru_list_.pop_back();
        cache_map_.erase(oldest.hash);
        if (oldest.texture_id != 0) {
            gl::glDeleteTextures(1, &oldest.texture_id);
        }
    }

    CachedTexture new_entry{ hash, tex_id, width, height };
    lru_list_.push_front(new_entry);
    cache_map_[hash] = lru_list_.begin();
    return new_entry;
}

} // namespace snp::gles3
