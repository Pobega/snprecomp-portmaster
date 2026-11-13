#include "combiner_shader_cache.h"
#include "n64_gbi.h"
#include <cstdio>
#include <sstream>
#include <vector>

namespace snp::gles3 {

namespace {
    std::string get_color_source(uint32_t val, const std::string& combined_name, bool is_c) {
        if (is_c) {
            switch (val) {
                case G_CCMUX_COMBINED: return combined_name + ".rgb";
                case G_CCMUX_TEXEL0: return "tex0.rgb";
                case G_CCMUX_TEXEL1: return "tex1.rgb";
                case G_CCMUX_PRIMITIVE: return "uPrimColor.rgb";
                case G_CCMUX_SHADE: return "vColor.rgb";
                case G_CCMUX_ENVIRONMENT: return "uEnvColor.rgb";
                case G_CCMUX_1: return "vec3(1.0)";
                case G_CCMUX_COMBINED_ALPHA: return "vec3(" + combined_name + ".a)";
                case G_CCMUX_TEXEL0_ALPHA: return "vec3(tex0.a)";
                case G_CCMUX_TEXEL1_ALPHA: return "vec3(tex1.a)";
                case G_CCMUX_PRIMITIVE_ALPHA: return "vec3(uPrimColor.a)";
                case G_CCMUX_SHADE_ALPHA: return "vec3(vColor.a)";
                case G_CCMUX_ENV_ALPHA: return "vec3(uEnvColor.a)";
                case G_CCMUX_PRIM_LOD_FRAC: return "vec3(uPrimLodFrac)";
                default: return "vec3(0.0)";
            }
        }
        switch (val) {
            case G_CCMUX_COMBINED: return combined_name + ".rgb";
            case G_CCMUX_TEXEL0: return "tex0.rgb";
            case G_CCMUX_TEXEL1: return "tex1.rgb";
            case G_CCMUX_PRIMITIVE: return "uPrimColor.rgb";
            case G_CCMUX_SHADE: return "vColor.rgb";
            case G_CCMUX_ENVIRONMENT: return "uEnvColor.rgb";
            case G_CCMUX_1: return "vec3(1.0)";
            default: return "vec3(0.0)";
        }
    }

    std::string get_alpha_source(uint32_t val, const std::string& combined_name, bool is_ac) {
        if (is_ac && val == 6) {
            return "uPrimLodFrac";
        }
        switch (val) {
            case G_ACMUX_COMBINED: return is_ac ? "0.0" : (combined_name + ".a");
            case G_ACMUX_TEXEL0: return "tex0.a";
            case G_ACMUX_TEXEL1: return "tex1.a";
            case G_ACMUX_PRIMITIVE: return "uPrimColor.a";
            case G_ACMUX_SHADE: return "vColor.a";
            case G_ACMUX_ENVIRONMENT: return "uEnvColor.a";
            case G_ACMUX_1: return "1.0";
            default: return "0.0";
        }
    }

    bool is_tex0_color(uint32_t c) {
        return c == G_CCMUX_TEXEL0 || c == G_CCMUX_TEXEL0_ALPHA;
    }

    bool is_tex1_color(uint32_t c) {
        return c == G_CCMUX_TEXEL1 || c == G_CCMUX_TEXEL1_ALPHA;
    }

    bool is_tex0_alpha(uint32_t a) {
        return a == G_ACMUX_TEXEL0;
    }

    bool is_tex1_alpha(uint32_t a) {
        return a == G_ACMUX_TEXEL1;
    }

    GLuint compile_shader(GLenum type, const std::string& source) {
        GLuint shader = gl::glCreateShader(type);
        const char* src_ptr = source.c_str();
        gl::glShaderSource(shader, 1, &src_ptr, nullptr);
        gl::glCompileShader(shader);

        GLint success = 0;
        gl::glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            GLint log_len = 0;
            gl::glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
            std::vector<char> log(log_len + 1);
            gl::glGetShaderInfoLog(shader, log_len, nullptr, log.data());
            std::fprintf(stderr, "[GLES3] Shader compilation failed:\n%s\nSource:\n%s\n", log.data(), source.c_str());
            gl::glDeleteShader(shader);
            return 0;
        }
        return shader;
    }
}

CombinerShaderCache::CombinerShaderCache() = default;

CombinerShaderCache::~CombinerShaderCache() {
    reset();
}

void CombinerShaderCache::reset() {
    for (auto& pair : programs_) {
        if (pair.second.program_id != 0 && gl::glDeleteProgram) {
            gl::glDeleteProgram(pair.second.program_id);
        }
    }
    programs_.clear();
}

const CombinerProgram& CombinerShaderCache::get_program(uint32_t combine_w0, uint32_t combine_w1,
                                                        uint32_t cycle_type, bool fog_enabled,
                                                        bool alpha_test_enabled) {
    CombinerKey key{ combine_w0, combine_w1, cycle_type, fog_enabled, alpha_test_enabled };
    auto it = programs_.find(key);
    if (it != programs_.end()) {
        return it->second;
    }

    CombinerProgram prog = compile_program(key);
    auto inserted = programs_.emplace(key, prog);
    return inserted.first->second;
}

std::string CombinerShaderCache::generate_vertex_shader() const {
    return R"(#version 300 es
layout(location = 0) in highp vec4 aPosition;
layout(location = 1) in mediump vec4 aColor;
layout(location = 2) in highp vec2 aTexCoord;

out mediump vec4 vColor;
out highp vec2 vTexCoord;
out mediump float vFogFactor;

uniform highp vec4 uProjection[4];
uniform highp vec4 uModelView[4];
uniform highp vec2 uTexScale;
uniform highp vec2 uTexOffset;
uniform mediump float uFogMult;
uniform mediump float uFogOffset;

void main() {
    mat4 proj = mat4(uProjection[0], uProjection[1], uProjection[2], uProjection[3]);
    mat4 mv = mat4(uModelView[0], uModelView[1], uModelView[2], uModelView[3]);
    highp vec4 eyePos = mv * aPosition;
    gl_Position = proj * eyePos;
    vColor = aColor;
    vTexCoord = aTexCoord * uTexScale + uTexOffset;
    highp float inv_w = (abs(gl_Position.w) > 0.0001) ? (1.0 / gl_Position.w) : 0.0;
    highp float ndc_z = clamp(gl_Position.z * inv_w, -1.0, 1.0);
    mediump float fog = ndc_z * uFogMult + uFogOffset;
    vFogFactor = clamp(fog / 255.0, 0.0, 1.0);
}
)";
}

std::string CombinerShaderCache::generate_fragment_shader(const CombinerKey& key, bool& out_uses_tex0, bool& out_uses_tex1) const {
    uint32_t w0 = key.combine_w0;
    uint32_t w1 = key.combine_w1;

    uint32_t a0 = (w0 >> 20) & 0x0Fu;
    uint32_t c0 = (w0 >> 15) & 0x1Fu;
    uint32_t aa0 = (w0 >> 12) & 0x07u;
    uint32_t ac0 = (w0 >> 9) & 0x07u;
    uint32_t a1 = (w0 >> 5) & 0x0Fu;
    uint32_t c1 = (w0 >> 0) & 0x1Fu;

    uint32_t b0 = (w1 >> 28) & 0x0Fu;
    uint32_t b1 = (w1 >> 24) & 0x0Fu;
    uint32_t aa1 = (w1 >> 21) & 0x07u;
    uint32_t ac1 = (w1 >> 18) & 0x07u;
    uint32_t d0 = (w1 >> 15) & 0x07u;
    uint32_t ab0 = (w1 >> 12) & 0x07u;
    uint32_t ad0 = (w1 >> 9) & 0x07u;
    uint32_t d1 = (w1 >> 6) & 0x07u;
    uint32_t ab1 = (w1 >> 3) & 0x07u;
    uint32_t ad1 = (w1 >> 0) & 0x07u;

    if (key.cycle_type == G_CYC_COPY) {
        out_uses_tex0 = true;
        out_uses_tex1 = false;
    } else if (key.cycle_type == G_CYC_FILL) {
        out_uses_tex0 = false;
        out_uses_tex1 = false;
    } else {
        bool cyc0_tex0 = is_tex0_color(a0) || is_tex0_color(b0) || is_tex0_color(c0) || is_tex0_color(d0) ||
                         is_tex0_alpha(aa0) || is_tex0_alpha(ab0) || is_tex0_alpha(ac0) || is_tex0_alpha(ad0);
        bool cyc0_tex1 = is_tex1_color(a0) || is_tex1_color(b0) || is_tex1_color(c0) || is_tex1_color(d0) ||
                         is_tex1_alpha(aa0) || is_tex1_alpha(ab0) || is_tex1_alpha(ac0) || is_tex1_alpha(ad0);

        bool cyc1_tex0 = false;
        bool cyc1_tex1 = false;
        if (key.cycle_type == G_CYC_2CYCLE) {
            cyc1_tex0 = is_tex0_color(a1) || is_tex0_color(b1) || is_tex0_color(c1) || is_tex0_color(d1) ||
                        is_tex0_alpha(aa1) || is_tex0_alpha(ab1) || is_tex0_alpha(ac1) || is_tex0_alpha(ad1);
            cyc1_tex1 = is_tex1_color(a1) || is_tex1_color(b1) || is_tex1_color(c1) || is_tex1_color(d1) ||
                        is_tex1_alpha(aa1) || is_tex1_alpha(ab1) || is_tex1_alpha(ac1) || is_tex1_alpha(ad1);
        }

        out_uses_tex0 = cyc0_tex0 || cyc1_tex0;
        out_uses_tex1 = cyc0_tex1 || cyc1_tex1;
    }

    std::ostringstream ss;
    ss << "#version 300 es\n";
    ss << "precision mediump float;\n";
    ss << "in mediump vec4 vColor;\n";
    ss << "in highp vec2 vTexCoord;\n";
    ss << "in mediump float vFogFactor;\n";
    ss << "out vec4 fragColor;\n";
    if (out_uses_tex0) {
        ss << "uniform sampler2D uTexture0;\n";
    }
    if (out_uses_tex1) {
        ss << "uniform sampler2D uTexture1;\n";
    }
    ss << "uniform vec4 uPrimColor;\n";
    ss << "uniform float uPrimLodFrac;\n";
    ss << "uniform vec4 uEnvColor;\n";
    ss << "uniform vec4 uFogColor;\n";
    ss << "uniform vec4 uBlendColor;\n";
    ss << "uniform float uAlphaThreshold;\n\n";

    ss << "void main() {\n";
    if (out_uses_tex0) {
        ss << "    vec4 tex0 = texture(uTexture0, vTexCoord);\n";
    } else {
        ss << "    vec4 tex0 = vec4(0.0);\n";
    }
    if (out_uses_tex1) {
        ss << "    vec4 tex1 = texture(uTexture1, vTexCoord);\n";
    } else {
        ss << "    vec4 tex1 = vec4(0.0);\n";
    }
    ss << "    vec4 combined;\n";

    if (key.cycle_type == G_CYC_COPY) {
        ss << "    combined = tex0;\n";
    } else if (key.cycle_type == G_CYC_FILL) {
        ss << "    combined = vColor;\n";
    } else {
        std::string str_a0 = get_color_source(a0, "vec4(0.0)", false);
        std::string str_b0 = get_color_source(b0, "vec4(0.0)", false);
        std::string str_c0 = get_color_source(c0, "vec4(0.0)", true);
        std::string str_d0 = get_color_source(d0, "vec4(0.0)", false);

        std::string str_aa0 = get_alpha_source(aa0, "vec4(0.0)", false);
        std::string str_ab0 = get_alpha_source(ab0, "vec4(0.0)", false);
        std::string str_ac0 = get_alpha_source(ac0, "vec4(0.0)", true);
        std::string str_ad0 = get_alpha_source(ad0, "vec4(0.0)", false);

        ss << "    vec3 col1 = (" << str_a0 << " - " << str_b0 << ") * " << str_c0 << " + " << str_d0 << ";\n";
        ss << "    float alpha1 = (" << str_aa0 << " - " << str_ab0 << ") * " << str_ac0 << " + " << str_ad0 << ";\n";
        ss << "    combined = vec4(col1, alpha1);\n";

        if (key.cycle_type == G_CYC_2CYCLE) {
            std::string str_a1 = get_color_source(a1, "combined", false);
            std::string str_b1 = get_color_source(b1, "combined", false);
            std::string str_c1 = get_color_source(c1, "combined", true);
            std::string str_d1 = get_color_source(d1, "combined", false);

            std::string str_aa1 = get_alpha_source(aa1, "combined", false);
            std::string str_ab1 = get_alpha_source(ab1, "combined", false);
            std::string str_ac1 = get_alpha_source(ac1, "combined", true);
            std::string str_ad1 = get_alpha_source(ad1, "combined", false);

            ss << "    vec3 col2 = (" << str_a1 << " - " << str_b1 << ") * " << str_c1 << " + " << str_d1 << ";\n";
            ss << "    float alpha2 = (" << str_aa1 << " - " << str_ab1 << ") * " << str_ac1 << " + " << str_ad1 << ";\n";
            ss << "    combined = vec4(col2, alpha2);\n";
        }
    }

    if (key.fog_enabled && key.cycle_type != G_CYC_COPY && key.cycle_type != G_CYC_FILL) {
        ss << "    combined.rgb = mix(combined.rgb, uFogColor.rgb, vFogFactor);\n";
    }

    if (key.alpha_test_enabled) {
        ss << "    if (combined.a < uAlphaThreshold) discard;\n";
    }

    ss << "    fragColor = clamp(combined, 0.0, 1.0);\n";
    ss << "}\n";

    return ss.str();
}

CombinerProgram CombinerShaderCache::compile_program(const CombinerKey& key) {
    CombinerProgram prog{};

    GLuint vs = compile_shader(GL_VERTEX_SHADER, generate_vertex_shader());
    if (!vs) return prog;

    bool uses_tex0 = false;
    bool uses_tex1 = false;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, generate_fragment_shader(key, uses_tex0, uses_tex1));
    if (!fs) {
        gl::glDeleteShader(vs);
        return prog;
    }

    GLuint program = gl::glCreateProgram();
    gl::glAttachShader(program, vs);
    gl::glAttachShader(program, fs);
    gl::glLinkProgram(program);

    GLint linked = 0;
    gl::glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint log_len = 0;
        gl::glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
        std::vector<char> log(log_len + 1);
        gl::glGetProgramInfoLog(program, log_len, nullptr, log.data());
        std::fprintf(stderr, "[GLES3] Program linking failed:\n%s\n", log.data());
        gl::glDeleteProgram(program);
        gl::glDeleteShader(vs);
        gl::glDeleteShader(fs);
        return prog;
    }

    gl::glDeleteShader(vs);
    gl::glDeleteShader(fs);

    prog.program_id = program;
    prog.uProjection = gl::glGetUniformLocation(program, "uProjection");
    prog.uModelView = gl::glGetUniformLocation(program, "uModelView");
    prog.uTexScale = gl::glGetUniformLocation(program, "uTexScale");
    prog.uTexOffset = gl::glGetUniformLocation(program, "uTexOffset");
    prog.uFogMult = gl::glGetUniformLocation(program, "uFogMult");
    prog.uFogOffset = gl::glGetUniformLocation(program, "uFogOffset");
    prog.uPrimColor = gl::glGetUniformLocation(program, "uPrimColor");
    prog.uPrimLodFrac = gl::glGetUniformLocation(program, "uPrimLodFrac");
    prog.uEnvColor = gl::glGetUniformLocation(program, "uEnvColor");
    prog.uFogColor = gl::glGetUniformLocation(program, "uFogColor");
    prog.uBlendColor = gl::glGetUniformLocation(program, "uBlendColor");
    prog.uAlphaThreshold = gl::glGetUniformLocation(program, "uAlphaThreshold");
    prog.uTexture0 = gl::glGetUniformLocation(program, "uTexture0");
    prog.uTexture1 = gl::glGetUniformLocation(program, "uTexture1");
    prog.uses_tex0 = uses_tex0;
    prog.uses_tex1 = uses_tex1;

    return prog;
}

} // namespace snp::gles3
