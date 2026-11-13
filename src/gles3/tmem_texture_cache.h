#pragma once

#include <cstdint>
#include <vector>
#include <list>
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
    bool loaded_via_tile = false;
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

class TmemTextureCache {
public:
    static constexpr size_t TMEM_SIZE = 4096;
    static constexpr size_t MAX_CACHE_ENTRIES = 512;
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

private:
    uint64_t compute_hash(const TileDescriptor& tile, uint32_t width, uint32_t height, uint32_t textfilt, uint32_t textlut) const;
    void decode_pixel(const TileDescriptor& tile, uint32_t x, uint32_t y, uint32_t textlut,
                      uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const;

    uint8_t tmem_[TMEM_SIZE];
    TileDescriptor tiles_[NUM_TILES];
    TextureImageState timg_;

    std::list<CachedTexture> lru_list_;
    std::unordered_map<uint64_t, std::list<CachedTexture>::iterator> cache_map_;
};

} // namespace snp::gles3
