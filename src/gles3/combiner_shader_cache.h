#pragma once

#include <cstdint>
#include <unordered_map>
#include <string>
#include "gl_loader.h"

namespace snp::gles3 {

struct CombinerProgram {
    GLuint program_id = 0;
    GLint uProjection = -1;
    GLint uModelView = -1;
    GLint uTexScale = -1;
    GLint uTexOffset = -1;
    GLint uFogMult = -1;
    GLint uFogOffset = -1;
    GLint uPrimColor = -1;
    GLint uPrimLodFrac = -1;
    GLint uEnvColor = -1;
    GLint uFogColor = -1;
    GLint uBlendColor = -1;
    GLint uAlphaThreshold = -1;
    GLint uTexture0 = -1;
    GLint uTexture1 = -1;
};

struct CombinerKey {
    uint32_t combine_w0 = 0;
    uint32_t combine_w1 = 0;
    uint32_t cycle_type = 0;
    bool fog_enabled = false;
    bool alpha_test_enabled = false;

    bool operator==(const CombinerKey& other) const {
        return combine_w0 == other.combine_w0 &&
               combine_w1 == other.combine_w1 &&
               cycle_type == other.cycle_type &&
               fog_enabled == other.fog_enabled &&
               alpha_test_enabled == other.alpha_test_enabled;
    }
};

struct CombinerKeyHash {
    size_t operator()(const CombinerKey& k) const {
        uint64_t h = (static_cast<uint64_t>(k.combine_w0) << 32) | k.combine_w1;
        h ^= (static_cast<uint64_t>(k.cycle_type) << 16);
        h ^= (k.fog_enabled ? 1ULL : 0ULL) << 24;
        h ^= (k.alpha_test_enabled ? 1ULL : 0ULL) << 25;
        h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ULL;
        h = (h ^ (h >> 27)) * 0x94d049bb133111ebULL;
        return static_cast<size_t>(h ^ (h >> 31));
    }
};

class CombinerShaderCache {
public:
    CombinerShaderCache();
    ~CombinerShaderCache();

    void reset();

    const CombinerProgram& get_program(uint32_t combine_w0, uint32_t combine_w1,
                                       uint32_t cycle_type, bool fog_enabled,
                                       bool alpha_test_enabled);

private:
    CombinerProgram compile_program(const CombinerKey& key);
    std::string generate_vertex_shader() const;
    std::string generate_fragment_shader(const CombinerKey& key) const;

    std::unordered_map<CombinerKey, CombinerProgram, CombinerKeyHash> programs_;
};

} // namespace snp::gles3
