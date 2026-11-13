#pragma once

#include <cstdint>
#include <vector>
#include <list>
#include <deque>
#include <unordered_map>
#include "gl_loader.h"
#include "n64_gbi.h"

namespace snp::gles3 {

struct TileDescriptor {
    uint8_t fmt = 0;
    uint8_t siz = 0;
    uint16_t line = 0;
    uint16_t tmem = 0;
    uint8_t palette = 0;
    bool clamp_s = false;
    bool mirror_s = false;
    uint8_t mask_s = 0;
    uint8_t shift_s = 0;
    bool clamp_t = false;
    bool mirror_t = false;
    uint8_t mask_t = 0;
    uint8_t shift_t = 0;
    uint16_t sl = 0;
    uint16_t tl = 0;
    uint16_t sh = 0;
    uint16_t th = 0;
};

struct TextureImageState {
    uint8_t fmt = 0;
    uint8_t siz = 0;
    uint32_t width = 0;
    uint32_t phys_addr = 0;
};

struct CachedTexture {
    uint64_t hash = 0;
    GLuint texture_id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct TextureCacheStats {
    uint32_t fast_hits = 0;
    uint32_t map_hits = 0;
    uint32_t uploads = 0;
};

class TmemTextureCache {
public:
    static constexpr size_t TMEM_SIZE = 4096;
    static constexpr size_t MAX_CACHE_ENTRIES = 1024;
    static constexpr size_t MAX_RECYCLED_TEXTURES = 128;
    static constexpr size_t NUM_TILES = 8;

    TmemTextureCache();
    ~TmemTextureCache();

    void reset();

    void set_timg(uint8_t fmt, uint8_t siz, uint32_t width, uint32_t phys_addr);
    void set_tile(uint32_t tile_idx, uint8_t fmt, uint8_t siz, uint16_t line, uint16_t tmem,
                  uint8_t palette, bool clamp_t, bool mirror_t, uint8_t mask_t, uint8_t shift_t,
                  bool clamp_s, bool mirror_s, uint8_t mask_s, uint8_t shift_s);
    void set_tile_size(uint32_t tile_idx, uint16_t sl, uint16_t tl, uint16_t sh, uint16_t th);

    void load_block(uint32_t tile_idx, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt, const uint8_t* rdram);
    void load_tile(uint32_t tile_idx, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt, const uint8_t* rdram);
    void load_tlut(uint32_t tile_idx, uint32_t count, const uint8_t* rdram);

    CachedTexture fetch_texture(uint32_t tile_idx, uint32_t textfilt, uint32_t textlut);

    const TileDescriptor& get_tile(uint32_t tile_idx) const { return tiles_[tile_idx & 7]; }
    const TextureImageState& get_timg() const { return timg_; }
    const uint8_t* get_tmem() const { return tmem_; }
    void decode_pixel(const TileDescriptor& tile, uint32_t x, uint32_t y, uint32_t textlut,
                      uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const;

    TextureCacheStats get_and_reset_stats();
    size_t recycled_pool_size() const { return recycled_textures_.size(); }

private:
    struct FastLookupSlot {
        uint64_t tmem_gen = 0;
        uint64_t tile_gen = 0;
        uint32_t textfilt = 0;
        uint32_t textlut = 0;
        bool valid = false;
        CachedTexture texture{};
        std::list<CachedTexture>::iterator lru_iter{};
    };

    struct RecycledTexture {
        uint32_t width = 0;
        uint32_t height = 0;
        GLuint texture_id = 0;
    };

    uint64_t compute_hash(const TileDescriptor& tile, uint32_t width, uint32_t height, uint32_t textfilt, uint32_t textlut) const;

    uint8_t tmem_[TMEM_SIZE];
    TileDescriptor tiles_[NUM_TILES];
    TextureImageState timg_;

    uint64_t tmem_generation_ = 1;
    uint64_t tile_generation_[NUM_TILES] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    FastLookupSlot fast_slots_[NUM_TILES];

    std::list<CachedTexture> lru_list_;
    std::unordered_map<uint64_t, std::list<CachedTexture>::iterator> cache_map_;
    std::deque<RecycledTexture> recycled_textures_;

    uint32_t tex_fast_count_ = 0;
    uint32_t tex_hit_count_ = 0;
    uint32_t tex_upload_count_ = 0;

    std::vector<uint8_t> decode_buffer_;
};

} // namespace snp::gles3
