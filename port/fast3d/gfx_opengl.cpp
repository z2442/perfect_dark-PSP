
#include <cstdint>
#include <GLES/gl.h>

// --- Global state for two-pass N64 combiner emulation ---
extern "C" volatile uint8_t g_force_two_pass; // 0/1: draw second pass using TEXEL1
extern "C" volatile uint8_t g_two_pass_mode; // 0=off, 1=decal(alpha), 2=modulate, 3=additive, 4=additive-alpha, 5=replace
extern "C" volatile float g_tex_s_scale[2];
extern "C" volatile float g_tex_t_scale[2];
extern "C" volatile float g_tex_s_offset[2];
extern "C" volatile float g_tex_t_offset[2];
static GLuint s_tex_id[2] = {0,0};            // last selected GL textures for tile 0/1
static GLuint s_last_bound_tex = 0;           // tracks most recent texture bound via select_texture
static bool s_last_use_alpha = true;           // last requested blend enable from set_use_alpha
static bool s_last_modulate  = false;          // last requested modulate flag from set_use_alpha
static GLenum s_current_depth_func = GL_LEQUAL; // tracked from set_depth_mode
static bool s_blend_enabled = true;            // shadow of GL_BLEND enable state
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <unordered_map>
#include <algorithm>

extern "C"{
#include <GLES/egl.h>
#include <GLES/gl.h>
}

#include "system.h"
#include <pspfpu.h>
#include <pspmath.h>

#include <math.h>

#if defined(__PSP__)
// Minimal PSPGL structures so we can peek at the default surface buffers.
struct pspgl_buffer {
    uint16_t refcount;
    uint16_t refpad;
    int8_t   mapcount;
    uint8_t  flags;
    uint8_t  pad[2];
    void    *unk0;
    void    *unk1;
    pspgl_buffer *next;
    pspgl_buffer *prev;
    void    *base;      // pixel data
    uint32_t size_bytes;
};

struct pspgl_surface {
    uint32_t stamp;
    uint32_t config;
    uint32_t unk_addr;
    uint16_t stride;    // line stride in pixels
    uint8_t  flags;
    uint8_t  pad;
    pspgl_buffer *draw;     // buffer currently used for drawing
    pspgl_buffer *display;  // buffer currently being displayed
    pspgl_buffer *depth;
    pspgl_buffer **drawp;
    pspgl_buffer **readp;
    uint32_t mask0;
    uint32_t mask1;
};

static inline void psp_clear_gl_errors(void) {
    while (glGetError() != GL_NO_ERROR) {}
}

static inline GLenum psp_check_gl_error(const char* op, int w, int h, GLenum fmt, GLenum type) {
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        const s32 level = (err == GL_OUT_OF_MEMORY) ? LOG_ERROR : LOG_WARNING;
        sysLogPrintf(level, "F3D PSP: %s failed (size=%dx%d fmt=0x%04x type=0x%04x, err=0x%04x)",
                     op, w, h, (unsigned)fmt, (unsigned)type, (unsigned)err);
    }
    return err;
}
#else
static inline void psp_clear_gl_errors(void) {}
static inline GLenum psp_check_gl_error(const char*, int, int, GLenum, GLenum) { return GL_NO_ERROR; }
#endif

#if defined(__GNUC__) || defined(__clang__)
static inline void gfx_prefetch_read(const void* ptr) {
    __builtin_prefetch(ptr, 0, 1);
}
#else
static inline void gfx_prefetch_read(const void*) {}
#endif


static inline void pdMatrixMode(GLenum mode) { glMatrixMode(mode); }
static inline void pdPushMatrix(void) { glPushMatrix(); }
static inline void pdPopMatrix(void) { glPopMatrix(); }
static inline void pdLoadIdentity(void) { glLoadIdentity(); }
static inline void pdLoadMatrixf(const float *m) { glLoadMatrixf(m); }
static inline void pdOrthof(GLfloat left, GLfloat right, GLfloat bottom, GLfloat top, GLfloat znear, GLfloat zfar) {
    glOrthof(left, right, bottom, top, znear, zfar);
}
static inline void pdScalef(GLfloat x, GLfloat y, GLfloat z) { glScalef(x, y, z); }
static inline void pdTranslatef(GLfloat x, GLfloat y, GLfloat z) { glTranslatef(x, y, z); }


// ---- CPU-side texture copies & compositor for two-cycle emulation ----
struct CpuTex {
    int w = 0, h = 0;
    uint32_t version = 0;           // increments on each upload to this GL tex id
    std::vector<uint16_t> rgba4444; // premultiplied RGBA4444 data, size = w*h
};

static std::unordered_map<GLuint, CpuTex> s_cpu_tex;   // GL tex id -> CPU copy
static uint32_t s_cpu_tex_generation = 1;              // global monotonically increasing version

struct CompositeKey { GLuint a, b; uint8_t mode; };
struct CompositeVal { GLuint gl_tex = 0; int w = 0, h = 0; uint32_t ver_a = 0, ver_b = 0; };

struct CompositeKeyHash {
    size_t operator()(const CompositeKey &k) const noexcept {
        return (size_t)k.a * 1315423911u ^ (size_t)k.b * 2654435761u ^ (size_t)k.mode;
    }
};
struct CompositeKeyEq {
    bool operator()(const CompositeKey &x, const CompositeKey &y) const noexcept {
        return x.a==y.a && x.b==y.b && x.mode==y.mode;
    }
};

static std::unordered_map<CompositeKey, CompositeVal, CompositeKeyHash, CompositeKeyEq> s_composites;

static inline uint16_t pack_rgba4444_pma(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    // Inputs expected premultiplied (r,g,b already scaled by a)
    return (uint16_t)(((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4));
}
static inline void unpack_rgba4444_pma(uint16_t p, uint8_t &r, uint8_t &g, uint8_t &b, uint8_t &a) {
    uint8_t R = (uint8_t)((p >> 12) & 0xF);
    uint8_t G = (uint8_t)((p >> 8)  & 0xF);
    uint8_t B = (uint8_t)((p >> 4)  & 0xF);
    uint8_t A = (uint8_t)( p        & 0xF);
    r = (uint8_t)((R << 4) | R);
    g = (uint8_t)((G << 4) | G);
    b = (uint8_t)((B << 4) | B);
    a = (uint8_t)((A << 4) | A);
}

// Blend two PMA RGBA8888 samples according to g_two_pass_mode. Returns PMA.
static inline void blend_two_pass_sample(uint8_t r0, uint8_t g0, uint8_t b0, uint8_t a0,
                                         uint8_t r1, uint8_t g1, uint8_t b1, uint8_t a1,
                                         uint8_t mode,
                                         uint8_t &r, uint8_t &g, uint8_t &b, uint8_t &a) {
    switch (mode) {
        default: // 1: decal PMA => src1 over src0
        case 1: {
            uint16_t inv = 255 - a1;
            r = (uint8_t)std::min(255, (int)r1 + ((int)r0 * inv + 127)/255);
            g = (uint8_t)std::min(255, (int)g1 + ((int)g0 * inv + 127)/255);
            b = (uint8_t)std::min(255, (int)b1 + ((int)b0 * inv + 127)/255);
            a = (uint8_t)std::min(255, (int)a1 + ((int)a0 * inv + 127)/255);
            break;
        }
        case 2: { // modulate (multiply)
            r = (uint8_t)(((int)r0 * (int)r1 + 127)/255);
            g = (uint8_t)(((int)g0 * (int)g1 + 127)/255);
            b = (uint8_t)(((int)b0 * (int)b1 + 127)/255);
            a = (uint8_t)(((int)a0 * (int)a1 + 127)/255);
            break;
        }
        case 3: // additive
        case 4: { // additive-alpha (weight baked into tex1)
            int rr = r0 + r1; if (rr>255) rr=255;
            int gg = g0 + g1; if (gg>255) gg=255;
            int bb = b0 + b1; if (bb>255) bb=255;
            int aa = a0 + a1; if (aa>255) aa=255;
            r = (uint8_t)rr; g = (uint8_t)gg; b = (uint8_t)bb; a = (uint8_t)aa;
            break;
        }
        case 5: { // replace
            r = r1; g = g1; b = b1; a = a1; break;
        }
    }
}

// Build or fetch a composite GL texture for (tex0, tex1, mode). Returns 0 on failure.
static GLuint get_or_build_composite(GLuint tex0, GLuint tex1, uint8_t mode) {
    if (!tex0 || !tex1) return 0;
    auto it0 = s_cpu_tex.find(tex0), it1 = s_cpu_tex.find(tex1);
    if (it0 == s_cpu_tex.end() || it1 == s_cpu_tex.end()) return 0; // need CPU copies
    const CpuTex &A = it0->second, &B = it1->second;
    if (A.w == 0 || A.h == 0 || B.w == 0 || B.h == 0) return 0;
    if (A.w != B.w || A.h != B.h) return 0; // equal-size only

    CompositeKey key{tex0, tex1, mode};
    auto itC = s_composites.find(key);
    if (itC != s_composites.end()) {
        CompositeVal &cv = itC->second;
        if (cv.ver_a == A.version && cv.ver_b == B.version && cv.gl_tex != 0)
            return cv.gl_tex;
    }

    // Build composite
    const int W = A.w, H = A.h;
    static std::vector<uint16_t> out4444; out4444.resize((size_t)W * (size_t)H);
    for (int i = 0; i < W*H; ++i) {
        uint8_t r0,g0,b0,a0, r1,g1,b1,a1, r,g,b,a;
        unpack_rgba4444_pma(A.rgba4444[(size_t)i], r0,g0,b0,a0);
        unpack_rgba4444_pma(B.rgba4444[(size_t)i], r1,g1,b1,a1);
        blend_two_pass_sample(r0,g0,b0,a0, r1,g1,b1,a1, mode, r,g,b,a);
        out4444[(size_t)i] = pack_rgba4444_pma(r,g,b,a);
    }

    // Upload composite
    CompositeVal cv{}; if (itC != s_composites.end()) cv = itC->second;
    if (cv.gl_tex == 0) glGenTextures(1, &cv.gl_tex);
    cv.w = W; cv.h = H; cv.ver_a = A.version; cv.ver_b = B.version;

    glBindTexture(GL_TEXTURE_2D, cv.gl_tex);
    s_last_bound_tex = cv.gl_tex;
    // Default to linear filtering for composites to avoid dependency on undeclared globals.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    psp_clear_gl_errors();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)W, (GLsizei)H, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, out4444.data());
    if (psp_check_gl_error("glTexImage2D composite", W, H, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4) != GL_NO_ERROR) {
        if (cv.gl_tex != 0) {
            glDeleteTextures(1, &cv.gl_tex);
            cv.gl_tex = 0;
        }
        return 0;
    }

    s_composites[key] = cv;
    return cv.gl_tex;
}

/* GLES 1.1 headers may not define GL_MIRRORED_REPEAT.
   Map it to the OES value or define the constant ourselves so
   the compile-time check in gfx_opengl_set_sampler_parameters works. */
#ifndef GL_MIRRORED_REPEAT
#  ifdef GL_MIRRORED_REPEAT_OES
#    define GL_MIRRORED_REPEAT GL_MIRRORED_REPEAT_OES
#  else
#    define GL_MIRRORED_REPEAT 0x8370
#  endif
#endif

// Fallback defines for texture env combine on GLES 1.1 implementations that may miss them
#ifndef GL_COMBINE
#  define GL_COMBINE 0x8570
#endif
#ifndef GL_COMBINE_RGB
#  define GL_COMBINE_RGB 0x8571
#endif
#ifndef GL_COMBINE_ALPHA
#  define GL_COMBINE_ALPHA 0x8572
#endif
#ifndef GL_PRIMARY_COLOR
#  define GL_PRIMARY_COLOR 0x8577
#endif
#ifndef GL_SRC0_RGB
#  define GL_SRC0_RGB 0x8580
#endif
#ifndef GL_OPERAND0_RGB
#  define GL_OPERAND0_RGB 0x8590
#endif
#ifndef GL_SRC1_RGB
#  define GL_SRC1_RGB 0x8581
#endif
#ifndef GL_OPERAND1_RGB
#  define GL_OPERAND1_RGB 0x8591
#endif
#ifndef GL_SRC0_ALPHA
#  define GL_SRC0_ALPHA 0x8588
#endif
#ifndef GL_OPERAND0_ALPHA
#  define GL_OPERAND0_ALPHA 0x8598
#endif

// Track support for texture_env_combine (various vendor spellings)
static bool s_has_texenv_combine = false;

// Fallback for polygon offset on GLES 1.1 implementations that might miss the token
#ifndef GL_POLYGON_OFFSET_FILL
#  define GL_POLYGON_OFFSET_FILL 0x8037
#endif

#include <PR/gbi.h>
#include "gfx_rendering_api.h"
#include "gfx_api.h"

static FilteringMode current_filter_mode = FILTER_LINEAR;

#define SCREEN_WIDTH  480  // Set the screen width (example value)
#define SCREEN_HEIGHT 272  // Set the screen height (example value)

static bool es_depth_test  = false; /* GL_DEPTH_TEST currently enabled? */
static bool es_depth_write = false;  /* TRUE if glDepthMask(GL_TRUE)      */

static float P_matrix[4][4]; // Global matrix for projection

static bool s_supports_depth_clamp = false;
static bool s_emulate_depth_clamp = true;
static const float kDepthClampScale = 0.3f; // matches desktop fallback when depth clamp is unavailable

extern "C" volatile uint8_t g_es1_depth_clamp_active;

// Forward state used by GLES1 framebuffer emulation and draw suppression
static int s_current_draw_fb = 0;           // 0 = default/backbuffer
static int s_system_game_fb_primary = -1;   // First created FB (used when game redirects main rendering)

// Minimal framebuffer emulation types placed early so they can be referenced
// from draw paths (e.g., to detect invert_y on bound framebuffer textures).
struct GLESFramebuffer {
    GLuint tex = 0;
    uint32_t w = 0, h = 0;
    bool invert_y = false;     // whether sampling should flip V
    bool allocated = false;
    bool valid = false;        // content up-to-date?
#if defined(__PSP__)
    uint32_t pot_w = 0, pot_h = 0; // power-of-two backing size on PSP
#endif
};

static std::vector<GLESFramebuffer> s_fbs; // index 0 is the default (window) fb

// --- 2D batch helpers: isolate state so 3D path stays untouched ---
static void begin_2d_batch() {
    g_es1_depth_clamp_active = 0;
    // Save PROJECTION and set screen-space ortho
    pdMatrixMode(GL_PROJECTION);
    pdPushMatrix();
    pdLoadIdentity();
    pdOrthof(0.0f, (GLfloat)SCREEN_WIDTH, (GLfloat)SCREEN_HEIGHT, 0.0f, -1.0f, 1.0f);

    // Save MODELVIEW and reset
    pdMatrixMode(GL_MODELVIEW);
    pdPushMatrix();
    pdLoadIdentity();

    // Disable depth test for pure 2D; remember we shadow the intended state in es_depth_*.
    if (es_depth_test) glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
}

static void end_2d_batch() {
    // Restore MODELVIEW then PROJECTION
    pdMatrixMode(GL_MODELVIEW);
    pdPopMatrix();

    pdMatrixMode(GL_PROJECTION);
    pdPopMatrix();

    // Restore depth state per shadow flags so 3D resumes exactly as before
    if (es_depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(es_depth_write ? GL_TRUE : GL_FALSE);

    // Return to MODELVIEW for client-state draws
    pdMatrixMode(GL_MODELVIEW);
}

extern "C" float g_es1_P[4][4];
extern "C" float g_es1_M[4][4];
extern "C" volatile int g_es1_matrix_dirty;
extern "C" volatile uint8_t g_es1_cull_mode; // 0=disable, 1=back, 2=front
// Alpha-test controls published by the RDP/RSP translator
extern "C" volatile uint8_t g_es1_alpha_test_enable; // 0/1
extern "C" volatile float   g_es1_alpha_test_ref;    // 0..1
extern "C" volatile uint8_t g_es1_highp_alpha;       // 0/1: request RGBA8888 uploads
extern "C" volatile uint8_t g_es1_tex0_in_rgb;       // 0/1: whether TEXEL0 contributes to RGB (hint)
extern "C" volatile uint8_t g_es1_force_2d;          // 0/1: force orthographic 2D for current batch
extern "C" volatile uint8_t g_es1_base_modulate;     // 0/1: base RGB uses SHADE (modulate) vs replace
extern "C" volatile uint8_t g_es1_base_color_mode;   // 0=none,1=shade,2=prim,3=env
extern "C" volatile uint8_t g_es1_prim_rgba[4];
extern "C" volatile uint8_t g_es1_env_rgba[4];
extern "C" volatile uint8_t g_es1_use_tex0;          // 0/1: whether TEXEL0 is used in this draw
extern "C" volatile uint8_t g_es1_use_tex1;          // 0/1: whether TEXEL1 is used in this draw
extern "C" volatile uint8_t g_es1_front_face_cw;     // 0/1: front faces are CW when mirrored
extern "C" volatile uint8_t g_es1_depth_clamp_active; // 0/1: projection currently applies depth clamp

static void glLoadRowMajorMatrixf(const float m[4][4]) {
    // Our matrices are laid out the way OpenGL expects already; load directly.
    pdLoadMatrixf(&m[0][0]);
}

static void load_projection_matrix_with_depth_clamp(const float m[4][4]) {
    if (s_supports_depth_clamp) {
        g_es1_depth_clamp_active = 1;
    } else if (!s_emulate_depth_clamp) {
        g_es1_depth_clamp_active = 0;
    }
    if (s_emulate_depth_clamp) {
        float adjusted[4][4];
        memcpy(adjusted, m, sizeof(adjusted));
        for (int i = 0; i < 4; ++i) {
            adjusted[i][2] *= kDepthClampScale;
        }
        pdLoadMatrixf(&adjusted[0][0]);
        g_es1_depth_clamp_active = 1;
    } else {
        pdLoadMatrixf(&m[0][0]);
        if (!s_supports_depth_clamp) {
            g_es1_depth_clamp_active = 0;
        }
    }
}

// --- 2D/3D projection helpers and UV mirroring ---
static bool s_is_2d_mode = false; // tracks currently selected projection mode

static inline float mirror_coord(float t) {
    // Mirror like GL_MIRRORED_REPEAT: odd integer tiles flip the fractional part
    float i = floorf(t);
    float f = t - i;
    // cast to int is safe here; only parity matters
    if (((int)i) & 1) return 1.0f - f;
    return f;
}


/* Add orthographic projection for 2D content */
static void gfx_opengl_set_orthographic_projection(float left, float right, float bottom, float top, float near_clip, float far_clip) {
    memset(P_matrix, 0, sizeof(P_matrix));
    P_matrix[0][0] = 2.0f / (right - left);
    P_matrix[1][1] = 2.0f / (top - bottom);
    P_matrix[2][2] = -2.0f / (far_clip - near_clip);
    P_matrix[3][0] = -(right + left) / (right - left);
    P_matrix[3][1] = -(top + bottom) / (top - bottom);
    P_matrix[3][2] = -(far_clip + near_clip) / (far_clip - near_clip);
    P_matrix[3][3] = 1.0f;
}


static void gfx_opengl_set_projection_for_2d() {
    pdMatrixMode(GL_PROJECTION);
    pdLoadIdentity();
    // Screen-space ortho: origin top-left, +x right, +y down
    pdOrthof(0.0f, (GLfloat)SCREEN_WIDTH, (GLfloat)SCREEN_HEIGHT, 0.0f, -1.0f, 1.0f);

    pdMatrixMode(GL_MODELVIEW);
    pdLoadIdentity();
}

static void gfx_opengl_set_projection_for_3d() {
    // Load external matrices provided by the engine
    pdMatrixMode(GL_PROJECTION);
    load_projection_matrix_with_depth_clamp(g_es1_P);
    pdMatrixMode(GL_MODELVIEW);
    glLoadRowMajorMatrixf(g_es1_M);
}


static uint16_t g_es_zmode = 0;

struct LoadedVertex {
    float x, y, z, w;   // Position
    float u, v;          // Texture coordinates
    uint8_t r, g, b, a;  // Color (RGBA)
};


struct ShaderProgram {
    GLuint opengl_program_id;
    uint8_t num_inputs;
    bool used_textures[2];
    uint8_t num_floats;
    GLint attrib_locations[16];
    uint8_t attrib_sizes[16];
    uint8_t num_attribs;
    GLint frame_count_location;
    GLint noise_scale_location;
    GLint three_point_filter_locations[2];
};

static bool current_textures_linear_filter[2] = {false, false};

static bool current_depth_mask = true;
static bool current_poly_offset = false;

// --- Texture env mode cache to avoid redundant state changes ---
enum TexEnvModeES1 { TEXENV_UNKNOWN=-1, TEXENV_MODULATE=0, TEXENV_REPLACE=1, TEXENV_FONT_COMBINE=2, TEXENV_MODULATE_CONST=3 };
static int s_texenv_mode = TEXENV_UNKNOWN;

static inline void set_texenv_modulate() {
    if (s_texenv_mode == TEXENV_MODULATE) return;
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    s_texenv_mode = TEXENV_MODULATE;
}

static inline void set_texenv_replace() {
    if (s_texenv_mode == TEXENV_REPLACE) return;
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    s_texenv_mode = TEXENV_REPLACE;
}

static inline void set_texenv_font_combine() {
    if (s_has_texenv_combine) {
        if (s_texenv_mode == TEXENV_FONT_COMBINE) return;
        // Color = primary, Alpha = texture alpha
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB,  GL_REPLACE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB,     GL_PRIMARY_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA,  GL_REPLACE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA,     GL_TEXTURE);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
        s_texenv_mode = TEXENV_FONT_COMBINE;
    } else {
        // Fallback: use MODULATE so RGB follows primary and alpha follows texture
        // (works well for white glyph textures). This avoids GL_COMBINE enums on
        // drivers without texture_env_combine support.
        if (s_texenv_mode != TEXENV_MODULATE) {
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        }
        s_texenv_mode = TEXENV_MODULATE;
    }
}

static inline void set_texenv_texture_modulate_with_constant(bool alpha_from_primary) {
    if (s_has_texenv_combine) {
        if (s_texenv_mode == TEXENV_MODULATE_CONST) {
            // Still update alpha combine in case it differs
        }
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        // RGB = TEXEL * PRIMARY
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_TEXTURE);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB, GL_PRIMARY_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
        // ALPHA = PRIMARY_ALPHA or TEXEL_ALPHA
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
        if (alpha_from_primary) {
            glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_PRIMARY_COLOR);
            glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
        } else {
            glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_TEXTURE);
            glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
        }
        s_texenv_mode = TEXENV_MODULATE_CONST;
    } else {
        // Fallback: simple MODULATE uses the current constant/vertex color
        // and multiplies it with the texture. This avoids unsupported enums.
        if (s_texenv_mode != TEXENV_MODULATE) {
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        }
        s_texenv_mode = TEXENV_MODULATE;
    }
}


static void gfx_opengl_unload_shader(struct ShaderProgram* old_prg) {}

static void gfx_opengl_load_shader(struct ShaderProgram* new_prg) {}

static struct ShaderProgram* gfx_opengl_create_and_load_new_shader(uint64_t shader_id0, uint32_t shader_id1) {
    static struct ShaderProgram dummy;
    memset(&dummy, 0, sizeof(dummy));
    dummy.used_textures[0] = true;
    return &dummy;
}

static struct ShaderProgram* gfx_opengl_lookup_shader(uint64_t shader_id0, uint32_t shader_id1) {
    return gfx_opengl_create_and_load_new_shader(shader_id0, shader_id1);
}

static void gfx_opengl_shader_get_info(struct ShaderProgram* prg, uint8_t* num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static void gfx_opengl_clear_shaders(void) {}

static GLuint gfx_opengl_new_texture(void) {
    GLuint tex;
    glGenTextures(1, &tex);
    return tex;
}

static void gfx_opengl_delete_texture(uint32_t texID) {
    GLuint gl_tex_id = (GLuint)texID;
    glDeleteTextures(1, &gl_tex_id);
}

static void gfx_opengl_select_texture(int tile, GLuint texture_id, bool linear_filter) {
    if (tile < 0) tile = 0;
    if (tile > 1) tile = 1;
    s_tex_id[tile] = texture_id;        // remember per "tile" index from RDP
    glBindTexture(GL_TEXTURE_2D, texture_id);
    s_last_bound_tex = texture_id;
    current_textures_linear_filter[tile] = linear_filter;
}

static void gfx_opengl_upload_texture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
#if defined(__PSP__)
        sysLogPrintf(LOG_WARNING, "F3D PSP: skipped texture upload with zero dimension (%ux%u)", (unsigned)width, (unsigned)height);
#endif
        return;
    }
    const size_t num_pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    constexpr size_t kPrefetchDistance = 16; // 1 cache line (in bytes) for sequential texture reads
#if defined(__PSP__)
    // PSP: prefer RGBA4444 uploads; fall back to RGBA8888 if required.
    static std::vector<uint16_t> rgba4444;
    static std::vector<uint8_t>  rgba8888;
    static bool psp_warned_8888 = false;

    rgba4444.resize(num_pixels);

    const uint8_t* src = rgba32_buf;
    for (size_t i = 0; i < num_pixels; ++i) {
        if ((i & 0xF) == 0) {
            gfx_prefetch_read(src + kPrefetchDistance);
        }
        uint8_t r = src[0];
        uint8_t g = src[1];
        uint8_t b = src[2];
        uint8_t a = src[3];
        src += 4;

        uint8_t pm_r = (uint8_t)((r * a + 128) >> 8);
        uint8_t pm_g = (uint8_t)((g * a + 128) >> 8);
        uint8_t pm_b = (uint8_t)((b * a + 128) >> 8);

        rgba4444[i] = (uint16_t)(((pm_r >> 4) << 12) | ((pm_g >> 4) << 8) | ((pm_b >> 4) << 4) | (a >> 4));
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    psp_clear_gl_errors();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 (GLsizei)width, (GLsizei)height, 0,
                 GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4,
                 rgba4444.data());
    GLenum upload_err = psp_check_gl_error("glTexImage2D upload (PSP RGBA4444)", (int)width, (int)height, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4);
    if (upload_err != GL_NO_ERROR) {
        // Fall back to RGBA8888 (premultiplied) if 16-bit upload fails
        rgba8888.resize(num_pixels * 4u);
        const uint16_t* src4444 = rgba4444.data();
        for (size_t i = 0; i < num_pixels; ++i) {
            if ((i & 0x1F) == 0) {
                gfx_prefetch_read(reinterpret_cast<const uint8_t*>(src4444 + i) + kPrefetchDistance);
            }
            uint16_t p = src4444[i];
            uint8_t r4 = (uint8_t)((p >> 12) & 0xF);
            uint8_t g4 = (uint8_t)((p >> 8)  & 0xF);
            uint8_t b4 = (uint8_t)((p >> 4)  & 0xF);
            uint8_t a4 = (uint8_t)(p & 0xF);
            rgba8888[i*4 + 0] = (uint8_t)((r4 << 4) | r4);
            rgba8888[i*4 + 1] = (uint8_t)((g4 << 4) | g4);
            rgba8888[i*4 + 2] = (uint8_t)((b4 << 4) | b4);
            rgba8888[i*4 + 3] = (uint8_t)((a4 << 4) | a4);
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        psp_clear_gl_errors();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     (GLsizei)width, (GLsizei)height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE,
                     rgba8888.data());
        if (psp_check_gl_error("glTexImage2D upload fallback (PSP RGBA8888)", (int)width, (int)height, GL_RGBA, GL_UNSIGNED_BYTE) != GL_NO_ERROR) {
            return;
        }
        if (!psp_warned_8888) {
            sysLogPrintf(LOG_WARNING, "F3D PSP: falling back to RGBA8888 textures; expect higher memory usage");
            psp_warned_8888 = true;
        }
    }
#else
    if (g_es1_highp_alpha) {
        if (s_has_texenv_combine) {
            // Fast font/UI: alpha-only, color from PRIMARY
            static std::vector<uint8_t> alpha8;
            alpha8.resize(num_pixels);
            const uint8_t* src = rgba32_buf + 3;
            uint8_t*       dst = alpha8.data();
            for (size_t i = 0; i < num_pixels; ++i) {
                if ((i & 0xF) == 0) {
                    gfx_prefetch_read(src + kPrefetchDistance);
                }
                dst[i] = *src;
                src += 4;
            }
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA,
                         width, height, 0,
                         GL_ALPHA, GL_UNSIGNED_BYTE,
                         alpha8.data());
        } else {
            // No COMBINE: use LA
            static std::vector<uint8_t> la;
            la.resize(num_pixels * 2);
            const uint8_t* src = rgba32_buf + 3;
            uint8_t*       dst = la.data();
            for (size_t i = 0; i < num_pixels; ++i) {
                if ((i & 0xF) == 0) {
                    gfx_prefetch_read(src + kPrefetchDistance);
                }
                dst[0] = 255;
                dst[1] = *src;
                src += 4;
                dst += 2;
            }
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA,
                         width, height, 0,
                         GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE,
                         la.data());
        }
    } else {
        // Premultiplied RGBA4444 — faster, smaller; good for most scene textures
        static std::vector<uint16_t> rgba16;
        rgba16.resize(num_pixels);
        const uint8_t* src = rgba32_buf;
        uint16_t*      dst = rgba16.data();
        size_t i = 0;
        const size_t n4 = num_pixels & ~static_cast<size_t>(3);
        for (; i < n4; i += 4) {
            if ((i & 0xF) == 0) {
                gfx_prefetch_read(src + kPrefetchDistance);
            }
            uint8_t r0 = src[0],  g0 = src[1],  b0 = src[2],  a0 = src[3];
            uint8_t r1 = src[4],  g1 = src[5],  b1 = src[6],  a1 = src[7];
            uint8_t r2 = src[8],  g2 = src[9],  b2 = src[10], a2 = src[11];
            uint8_t r3 = src[12], g3 = src[13], b3 = src[14], a3 = src[15];
            src += 16;
            r0 = (uint8_t)((r0 * a0 + 128) >> 8); g0 = (uint8_t)((g0 * a0 + 128) >> 8); b0 = (uint8_t)((b0 * a0 + 128) >> 8);
            r1 = (uint8_t)((r1 * a1 + 128) >> 8); g1 = (uint8_t)((g1 * a1 + 128) >> 8); b1 = (uint8_t)((b1 * a1 + 128) >> 8);
            r2 = (uint8_t)((r2 * a2 + 128) >> 8); g2 = (uint8_t)((g2 * a2 + 128) >> 8); b2 = (uint8_t)((b2 * a2 + 128) >> 8);
            r3 = (uint8_t)((r3 * a3 + 128) >> 8); g3 = (uint8_t)((g3 * a3 + 128) >> 8); b3 = (uint8_t)((b3 * a3 + 128) >> 8);
            dst[0] = (uint16_t)(((r0 >> 4) << 12) | ((g0 >> 4) << 8) | ((b0 >> 4) << 4) | (a0 >> 4));
            dst[1] = (uint16_t)(((r1 >> 4) << 12) | ((g1 >> 4) << 8) | ((b1 >> 4) << 4) | (a1 >> 4));
            dst[2] = (uint16_t)(((r2 >> 4) << 12) | ((g2 >> 4) << 8) | ((b2 >> 4) << 4) | (a2 >> 4));
            dst[3] = (uint16_t)(((r3 >> 4) << 12) | ((g3 >> 4) << 8) | ((b3 >> 4) << 4) | (a3 >> 4));
            dst += 4;
        }
        for (; i < num_pixels; ++i) {
            if ((i & 0xF) == 0) {
                gfx_prefetch_read(src + kPrefetchDistance);
            }
            uint8_t r = src[0], g = src[1], b = src[2], a = src[3];
            src += 4;
            r = (uint8_t)((r * a + 128) >> 8);
            g = (uint8_t)((g * a + 128) >> 8);
            b = (uint8_t)((b * a + 128) >> 8);
            *dst++ = (uint16_t)(((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4));
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        psp_clear_gl_errors();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     (GLsizei)width, (GLsizei)height, 0,
                     GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4,
                     rgba16.data());
        if (psp_check_gl_error("glTexImage2D upload (RGBA4444)", (int)width, (int)height, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4) != GL_NO_ERROR) {
            return;
        }
    }
#endif

    // --- Record/update CPU copy for compositor ---
    GLuint bound_tex = 0;
#if defined(__PSP__)
    bound_tex = s_last_bound_tex;
#else
    GLint current_binding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &current_binding);
    bound_tex = (GLuint)current_binding;
#endif
    if (bound_tex != 0) {
        CpuTex &ct = s_cpu_tex[bound_tex];
        ct.w = (int)width; ct.h = (int)height;
        ct.version = ++s_cpu_tex_generation;
        ct.rgba4444.resize((size_t)width * (size_t)height);
#if defined(__PSP__)
        // We already have RGBA4444 premultiplied in `rgba4444`
        memcpy(ct.rgba4444.data(), rgba4444.data(), (size_t)width * (size_t)height * sizeof(uint16_t));
#else
        // Ensure a PMA RGBA4444 copy regardless of upload path
        const size_t num_pixels = (size_t)width * (size_t)height;
        const uint8_t* src = rgba32_buf;
        uint16_t* dst = ct.rgba4444.data();
        for (size_t i = 0; i < num_pixels; ++i) {
            if ((i & 0xF) == 0) {
                gfx_prefetch_read(src + kPrefetchDistance);
            }
            uint8_t r = src[0], g = src[1], b = src[2], a = src[3];
            src += 4;
            r = (uint8_t)((r * a + 128) >> 8);
            g = (uint8_t)((g * a + 128) >> 8);
            b = (uint8_t)((b * a + 128) >> 8);
            *dst++ = pack_rgba4444_pma(r,g,b,a);
        }
#endif
    }

}

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    switch (val) {
        case G_TX_CLAMP:
            return GL_CLAMP_TO_EDGE;

        case G_TX_WRAP:
            return GL_REPEAT;

    }
    return GL_REPEAT; // Default fallback
}

static void gfx_opengl_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    const GLint filter = (linear_filter && current_filter_mode == FILTER_LINEAR) ? GL_LINEAR : GL_NEAREST;

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

    // Software MIRROR path: texture was expanded to a 2x pair when MIRROR is set.
    // Always use GL_REPEAT in those axes so the [orig|mirror] pair repeats identically to N64.
    const bool mirror_s = (cms & G_TX_MIRROR) != 0;
    const bool mirror_t = (cmt & G_TX_MIRROR) != 0;
    GLenum wrap_s = mirror_s ? GL_REPEAT : ((cms & G_TX_CLAMP) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    GLenum wrap_t = mirror_t ? GL_REPEAT : ((cmt & G_TX_CLAMP) ? GL_CLAMP_TO_EDGE : GL_REPEAT);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_s);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_t);
}

static void gfx_opengl_set_depth_mode(bool depth_test, bool depth_update, bool depth_compare, bool depth_source_prim, uint16_t zmode) {
    es_depth_test  = depth_test;
    es_depth_write = depth_update;
    if (depth_test) {
        glEnable(GL_DEPTH_TEST);
        glDepthMask(depth_update ? GL_TRUE : GL_FALSE);
        current_depth_mask = depth_update;

        if (depth_compare) {
            switch (zmode) {
                case ZMODE_INTER:
                    glDepthFunc(GL_LEQUAL);
                    s_current_depth_func = GL_LEQUAL;
                    if (current_poly_offset) { glDisable(GL_POLYGON_OFFSET_FILL); current_poly_offset = false; }
                    break;
                case ZMODE_OPA:
                case ZMODE_XLU:
                    if (depth_source_prim) {
                        glDepthFunc(GL_LEQUAL);
                        s_current_depth_func = GL_LEQUAL;
                    } else {
                        glDepthFunc(GL_LESS);
                        s_current_depth_func = GL_LESS;
                    }
                    if (current_poly_offset) { glDisable(GL_POLYGON_OFFSET_FILL); current_poly_offset = false; }
                    break;
                case ZMODE_DEC:
                    glDepthFunc(GL_LEQUAL);
                    s_current_depth_func = GL_LEQUAL;
                    // Emulate N64 decal behavior by pulling coplanar overlays slightly toward the camera
                    // to avoid z-fighting. This is supported in GLES 1.1 via GL_POLYGON_OFFSET_FILL.
                    // Use a small negative bias; tune if needed.
                    glEnable(GL_POLYGON_OFFSET_FILL);
                    glPolygonOffset(-1.0f, -1.0f);
                    current_poly_offset = true;
                    break;
            }
        } else {
            glDepthFunc(GL_ALWAYS);
            s_current_depth_func = GL_ALWAYS;
            if (current_poly_offset) { glDisable(GL_POLYGON_OFFSET_FILL); current_poly_offset = false; }
        }
    } else {
        glDisable(GL_DEPTH_TEST);
        if (current_poly_offset) { glDisable(GL_POLYGON_OFFSET_FILL); current_poly_offset = false; }
    }
}

static float gfx_adjust_x_for_aspect_ratio(float x) {
    // This function can be modified to adjust x based on your aspect ratio needs
    float aspect_ratio = (float)SCREEN_WIDTH / (float)SCREEN_HEIGHT;
    return x * aspect_ratio;
}


static void gfx_opengl_set_depth_range(float znear, float zfar) {
    glDepthRangef(znear, zfar);
}

static void gfx_opengl_set_viewport(int x, int y, int width, int height) {
    glViewport(x, y, width, height);
}

static void gfx_opengl_set_scissor(int x, int y, int width, int height) {
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, width, height);
}

static void gfx_opengl_set_use_alpha(bool use_alpha, bool modulate) {
    // Respect caller's desired blend enable
    if (use_alpha) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }

    // Use premultiplied-alpha friendly blending in the default (non-modulate) path
    // since we upload textures as PMA in gfx_opengl_upload_texture.
    if (modulate) {
        // Multiplicative blending (detail textures, shadows): dest * srcColor
        glBlendFunc(GL_DST_COLOR, GL_ZERO);
    } else {
        // Standard translucent using PMA: out = src + dst*(1-src.a)
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }

    s_last_use_alpha = use_alpha;
    s_last_modulate  = modulate;
    s_blend_enabled  = use_alpha;
}

static inline void es11_apply_tex_transform(int tile) {
    pdMatrixMode(GL_TEXTURE);
    pdLoadIdentity();
    pdScalef(g_tex_s_scale[tile], g_tex_t_scale[tile], 1.0f);
    pdTranslatef(g_tex_s_offset[tile], g_tex_t_offset[tile], 0.0f);
    pdMatrixMode(GL_MODELVIEW);
}


static void gfx_opengl_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    // Suppress draws when targeting custom offscreen FBOs we cannot render to.
    // Allow draws when targeting the system primary offscreen (the game’s main render target),

    const int stride_floats = 9; // pos(3) + uv(2) + color(4)
    const int stride_bytes = stride_floats * sizeof(float);

    // Install P/M or temporary ortho for forced 2D batches
    bool forced2D = (g_es1_force_2d != 0);
    bool prevDepthTestLocal = es_depth_test;
    bool prevDepthMaskLocal = current_depth_mask;
    if (forced2D) {
        pdMatrixMode(GL_PROJECTION);
        pdPushMatrix();
        pdLoadIdentity();
        // NDC ortho to match N64-style UI math (-1..1)
        pdOrthof(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);
        pdMatrixMode(GL_MODELVIEW);
        pdPushMatrix();
        pdLoadIdentity();
        // Disable depth test and writes for UI
        if (prevDepthTestLocal) glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        // 2D shouldn't be culled
        glDisable(GL_CULL_FACE);
    } else {
        if (g_es1_matrix_dirty) {
            pdMatrixMode(GL_PROJECTION);
            load_projection_matrix_with_depth_clamp(g_es1_P);
            pdMatrixMode(GL_MODELVIEW);
            glLoadRowMajorMatrixf(g_es1_M);
            g_es1_matrix_dirty = 0;
        }
    }

    // Depth test state is managed by set_depth_mode; do not toggle here.

    // Keep CCW winding; CPU flips per-limb if mirrored.
    //glFrontFace(GL_CCW);
    // Apply N64 cull mode (was disabled above for forced 2D)
    if (!forced2D && g_es1_cull_mode == 0) {
        glDisable(GL_CULL_FACE);
    } else if (!forced2D) {
        glEnable(GL_CULL_FACE);
        glCullFace(g_es1_cull_mode == 1 ? GL_BACK : GL_FRONT);
    }

    pdMatrixMode(GL_MODELVIEW);

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    bool colorArrayEnabled = true;
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_FLOAT, stride_bytes, buf_vbo + 5);

    glVertexPointer(3, GL_FLOAT, stride_bytes, buf_vbo);
    glTexCoordPointer(2, GL_FLOAT, stride_bytes, buf_vbo + 3);

    // Enable texturing only if TEXEL0 is used
    if (g_es1_use_tex0) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);

    // Snapshot state affected inside this routine so we can restore reliably
    const bool   prevBlend      = s_blend_enabled;            // from set_use_alpha()
    const bool   prevDepthMask  = current_depth_mask;         // from set_depth_mode()
    const GLenum prevDepthFunc  = s_current_depth_func;       // from set_depth_mode()

    // --- Pass 1: base (TEXEL0) ---
    if (g_es1_use_tex0 && s_tex_id[0] != 0) {
        glBindTexture(GL_TEXTURE_2D, s_tex_id[0]);
        // If TEXEL0 is a framebuffer texture with inverted V, apply via texture matrix.
        // Important: if the framebuffer uses a POT texture, only the [0..t_max] sub-rect is valid;
        // flip within that range so we don't sample uninitialized padding.
        bool needs_invert_v = false;
        float t_max = 1.0f;
        for (const auto &fb : s_fbs) {
            if (fb.allocated && fb.tex == s_tex_id[0] && fb.invert_y) {
                needs_invert_v = false;
                const float ph = (float)(fb.pot_h ? fb.pot_h : fb.h);
                t_max = (ph > 0.0f) ? ((float)fb.h / ph) : 1.0f;
                break;
            }
        }
        if (needs_invert_v) {
            pdMatrixMode(GL_TEXTURE);
            pdLoadIdentity();
            pdTranslatef(0.0f, t_max, 0.0f);
            pdScalef(1.0f, -1.0f, 1.0f);
            pdMatrixMode(GL_MODELVIEW);
        }
    }
    // Base pass texenv:
    // - Font/UI glyphs: color = SHADE (primary), alpha = TEXEL0.a
    // - If RGB combine uses SHADE, use MODULATE; if PRIM/ENV, MODULATE with constant; else REPLACE
    if (g_es1_use_tex0 && g_es1_highp_alpha && g_es1_alpha_test_enable && !g_es1_tex0_in_rgb) {
        set_texenv_font_combine();
    } else {
        switch (g_es1_base_color_mode) {
            default:
            case 0: // none
                if (g_es1_use_tex0) set_texenv_replace(); else {
                    // No texture; choose constant or shade color only
                    if (g_es1_base_color_mode == 1) {
                        // shade only; keep color array enabled
                    } else {
                        // prim/env only: disable color array and set constant
                        glDisableClientState(GL_COLOR_ARRAY);
                        colorArrayEnabled = false;
                        uint8_t r8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[0] : g_es1_env_rgba[0];
                        uint8_t g8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[1] : g_es1_env_rgba[1];
                        uint8_t b8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[2] : g_es1_env_rgba[2];
                        uint8_t a8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[3] : g_es1_env_rgba[3];
                        float af = a8 / 255.0f;
                        glColor4f((r8 / 255.0f) * af, (g8 / 255.0f) * af, (b8 / 255.0f) * af, af);
                    }
                }
                break;
            case 1: // shade
                if (g_es1_use_tex0) set_texenv_modulate(); // else rely on per-vertex SHADE via color array
                break;
            case 2: // prim
            case 3: // env
                if (g_es1_use_tex0) {
                    // RGB = TEXEL0 * const; Alpha typically from const.
                    // For both COMBINE and fallback MODULATE, feed the constant via PRIMARY color.
                    // Important: use NON-premultiplied primary color so we don't double-multiply
                    // against PMA textures when MODULATE is used as a fallback.
                    glDisableClientState(GL_COLOR_ARRAY);
                    colorArrayEnabled = false;
                    uint8_t r8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[0] : g_es1_env_rgba[0];
                    uint8_t g8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[1] : g_es1_env_rgba[1];
                    uint8_t b8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[2] : g_es1_env_rgba[2];
                    uint8_t a8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[3] : g_es1_env_rgba[3];
                    glColor4f(r8 / 255.0f, g8 / 255.0f, b8 / 255.0f, a8 / 255.0f);
                    const bool font_like = g_es1_highp_alpha && g_es1_alpha_test_enable;
                    set_texenv_texture_modulate_with_constant(!font_like);
                } else {
                    // Constant color only; pre-multiply for PMA blending.
                    glDisableClientState(GL_COLOR_ARRAY);
                    colorArrayEnabled = false;
                    uint8_t r8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[0] : g_es1_env_rgba[0];
                    uint8_t g8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[1] : g_es1_env_rgba[1];
                    uint8_t b8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[2] : g_es1_env_rgba[2];
                    uint8_t a8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[3] : g_es1_env_rgba[3];
                    float af = a8 / 255.0f;
                    glColor4f((r8 / 255.0f) * af, (g8 / 255.0f) * af, (b8 / 255.0f) * af, af);
                }
                break;
        }
    }

    // For two-pass emulation we draw the base unblended; otherwise honor current blend state
    // Never run two-pass overlay for fonts/UI (alpha-tested + highp alpha)
// Prefer CPU-side composite to avoid second pass when possible
bool want_two_pass = (g_force_two_pass != 0) && g_es1_use_tex1 && (s_tex_id[1] != 0) && !(g_es1_highp_alpha && g_es1_alpha_test_enable);
bool using_two_pass = want_two_pass;
GLuint composite_tex = 0;
if (want_two_pass) {
    composite_tex = get_or_build_composite(s_tex_id[0], s_tex_id[1], (uint8_t)g_two_pass_mode);
    if (composite_tex != 0) {
        glBindTexture(GL_TEXTURE_2D, composite_tex);
        s_tex_id[0] = composite_tex; // ensure later code sees the bound texture
        using_two_pass = false;
    }
}

if (using_two_pass) {
    glDisable(GL_BLEND);
} else {
        bool want_blend = prevBlend;

        if (g_es1_highp_alpha && g_es1_alpha_test_enable) {
            uint8_t const_alpha = 255;

            if (g_es1_base_color_mode == 2) {
                const_alpha = g_es1_prim_rgba[3];
            } else if (g_es1_base_color_mode == 3) {
                const_alpha = g_es1_env_rgba[3];
            }

            if (const_alpha < 255) {
                want_blend = true;
            } else if (!prevBlend) {
                want_blend = false;
            }
        }

        if (want_blend) {
            glEnable(GL_BLEND);

            if (s_last_modulate) {
                glBlendFunc(GL_DST_COLOR, GL_ZERO);
            } else {
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            }
        } else {
            glDisable(GL_BLEND);
        }

        // Keep blend func as set by gfx_opengl_set_use_alpha
        // Apply alpha testing when requested by RDP state, otherwise keep a minimal discard when blending
        const bool want_alpha_test = g_es1_alpha_test_enable || (want_blend && !s_last_modulate);
        float alpha_ref = g_es1_alpha_test_enable ? g_es1_alpha_test_ref : 0.0f;
        // Bias slightly to account for 4-bit/quantized sources and sampling
        if (alpha_ref > 0.0f && alpha_ref < 1.0f) alpha_ref = fmaxf(0.0f, alpha_ref - 0.03f);
        if (want_alpha_test) {
            glEnable(GL_ALPHA_TEST);
            glAlphaFunc(GL_GEQUAL, alpha_ref);
        }
    }

    glDepthMask(prevDepthMask ? GL_TRUE : GL_FALSE);
    glDepthFunc(prevDepthFunc);

    glDrawArrays(GL_TRIANGLES, 0, buf_vbo_num_tris * 3);

    if (!using_two_pass) {
        glDisable(GL_ALPHA_TEST);
    }

    // --- Optional Pass 2: overlay (TEXEL1) to emulate N64 dual-texture ---
    if (using_two_pass) {
        // Draw the exact same geometry again, using destination color as base
        // and blend mode based on s_last_modulate (ignore s_last_use_alpha).
        glBindTexture(GL_TEXTURE_2D, s_tex_id[1]);
        // Use texture1's alpha directly for blending (ignore vertex color alpha)
        set_texenv_replace();

        // Depth-equal to avoid z-fighting and keep z-buffer untouched
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_EQUAL);

        // Always blend in pass 2 so TEXEL1 can respect its alpha.
        glEnable(GL_BLEND);
        float alphaThreshold = 0.01f; // default low threshold
        switch (g_two_pass_mode) {
            default: // 0 or unknown -> treat as decal
            case 1: // Decal (premultiplied): src already includes alpha
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                alphaThreshold = 0.25f; // softer edge with PMA
                break;
            case 2: // Modulate: base * tex1
                glBlendFunc(GL_DST_COLOR, GL_ZERO);
                alphaThreshold = 0.0f; // keep as much detail as possible
                break;
            case 3: // Additive: base + tex1 (clamped)
                glBlendFunc(GL_ONE, GL_ONE);
                alphaThreshold = 0.0f; // do not clip; preserve glow tails
                break;
            case 4: // Additive-Alpha (premultiplied): weight baked into color
                glBlendFunc(GL_ONE, GL_ONE);
                alphaThreshold = 0.0f; // keep glow tails intact
                break;
            case 5: // Replace: TEXEL1 fully overwrites base (no blending)
                glDisable(GL_BLEND);
                s_blend_enabled = false;
                alphaThreshold = 0.0f; // no alpha test
                break;
        }
        s_blend_enabled = (g_two_pass_mode != 5);

        // Alpha test tuned per mode, consider global alpha-test request as a floor
        if (g_es1_alpha_test_enable) {
            if (g_es1_alpha_test_ref > alphaThreshold) alphaThreshold = g_es1_alpha_test_ref;
        }
        // Bias slightly to accommodate quantized alpha inputs
        if (alphaThreshold > 0.0f && alphaThreshold < 1.0f) alphaThreshold = fmaxf(0.0f, alphaThreshold - 0.03f);
        if (alphaThreshold > 0.0f) {
            glEnable(GL_ALPHA_TEST);
            glAlphaFunc(GL_GEQUAL, alphaThreshold);
        } else {
            glDisable(GL_ALPHA_TEST);
        }

        glDrawArrays(GL_TRIANGLES, 0, buf_vbo_num_tris * 3);

        // If we enabled alpha test for decals, turn it back off now
        if (alphaThreshold > 0.0f) {
            glDisable(GL_ALPHA_TEST);
        }

        // Restore depth write & func and prior blend enable + blend function
        glDepthMask(prevDepthMask ? GL_TRUE : GL_FALSE);
        glDepthFunc(prevDepthFunc);
        if (prevBlend) {
            glEnable(GL_BLEND);
            if (s_last_modulate) {
                glBlendFunc(GL_DST_COLOR, GL_ZERO);
            } else {
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            }
        } else {
            glDisable(GL_BLEND);
        }
        s_blend_enabled = prevBlend; // keep shadow in sync

        // Rebind TEXEL0 and restore base texenv for subsequent single-pass draws
        if (s_tex_id[0] != 0) glBindTexture(GL_TEXTURE_2D, s_tex_id[0]);
        if (g_es1_highp_alpha && g_es1_alpha_test_enable && !g_es1_tex0_in_rgb) set_texenv_font_combine(); else if (g_es1_base_modulate) set_texenv_modulate(); else set_texenv_replace();
    }

    pdMatrixMode(GL_TEXTURE);
    pdLoadIdentity();
    pdMatrixMode(GL_MODELVIEW);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);

    // Clean up per-draw leakage
    glDisable(GL_ALPHA_TEST);
    if (!colorArrayEnabled) {
        glEnableClientState(GL_COLOR_ARRAY);
    }
    if (forced2D) {
        // Restore matrices
        pdMatrixMode(GL_MODELVIEW);
        pdPopMatrix();
        pdMatrixMode(GL_PROJECTION);
        pdPopMatrix();
        pdMatrixMode(GL_MODELVIEW);
        // Restore depth
        if (prevDepthTestLocal) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        glDepthMask(prevDepthMaskLocal ? GL_TRUE : GL_FALSE);
    }
    // Depth test restored by set_depth_mode elsewhere
}

static const EGLint attrib_list [] = {
    EGL_SURFACE_TYPE,   EGL_WINDOW_BIT,
    EGL_RED_SIZE,       5,
    EGL_GREEN_SIZE,     6,
    EGL_BLUE_SIZE,      5,
    EGL_ALPHA_SIZE,     0,
    EGL_DEPTH_SIZE,     16,   // request a depth buffer (we use depth test)
    EGL_STENCIL_SIZE,   0,
    EGL_NONE
};

    EGLDisplay dpy;
	EGLConfig config;
	EGLint num_configs;
	EGLContext ctx;
	EGLSurface surface;
	GLfloat angle = 0.0f;

static void gfx_opengl_init(void) {


	/* pass NativeDisplay=0, we only have one screen... */
	dpy = eglGetDisplay(0);
	eglInitialize(dpy, NULL, NULL);

	eglChooseConfig(dpy, attrib_list, &config, 1, &num_configs);
    const EGLint ctx_attribs[] = {
        1,  // OpenGL ES 1.x context
        EGL_NONE
    };
    ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attribs);
	surface = eglCreateWindowSurface(dpy, config, 0, NULL);
	eglMakeCurrent(dpy, surface, surface, ctx);

    eglSwapInterval(dpy, 1);

    glViewport(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    glScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);

    glEnable(GL_TEXTURE_2D);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    // Default to PMA-friendly blending; set_use_alpha will override as needed
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    // Default to CCW so front faces aren't culled
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glCullFace(GL_BACK);

    // Match N64/desktop behavior: smooth shading, dither, and best perspective correction.
    glShadeModel(GL_SMOOTH);
    glEnable(GL_DITHER);
#if defined(GL_PERSPECTIVE_CORRECTION_HINT) && !defined(__PSP__)
    glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
#endif

    pdMatrixMode(GL_PROJECTION);
    pdLoadIdentity();
    pdMatrixMode(GL_MODELVIEW);
    pdLoadIdentity();

    glClearColor(0.0, 0.0, 0.0, 1.0);

    // Detect texture_env_combine support at runtime to avoid invalid enums
    const char* ext = (const char*) glGetString(GL_EXTENSIONS);
    if (ext) {
        if (strstr(ext, "GL_OES_texture_env_combine") ||
            strstr(ext, "GL_ARB_texture_env_combine") ||
            strstr(ext, "GL_EXT_texture_env_combine") ||
            strstr(ext, "GL_NV_texture_env_combine4") ||
            strstr(ext, "GL_APPLE_texture_env_combine")) {
            s_has_texenv_combine = true;
        }

        if (strstr(ext, "GL_NV_depth_clamp") ||
            strstr(ext, "GL_EXT_depth_clamp") ||
            strstr(ext, "GL_ARB_depth_clamp") ||
            strstr(ext, "GL_OES_depth_clamp")) {
            s_supports_depth_clamp = true;
        }
    }

    if (s_supports_depth_clamp) {
#ifdef GL_DEPTH_CLAMP
        glEnable(GL_DEPTH_CLAMP);
#endif
        s_emulate_depth_clamp = false;
    }

    // Set state trackers to match initial GL state
    s_last_use_alpha = true;
    s_last_modulate  = false;
    s_current_depth_func = GL_LEQUAL;
    s_texenv_mode = TEXENV_UNKNOWN;
}

static void gfx_opengl_end_frame(void) { glFlush();
eglSwapBuffers(dpy, surface); }
// --- Provide a definition for the two-pass flag so linker finds it if not provided elsewhere ---
extern "C" volatile uint8_t g_force_two_pass = 0;
extern "C" volatile uint8_t g_two_pass_mode = 0; // 0=off, 1=decal(alpha), 2=modulate, 3=additive, 4=additive-alpha, 5=replace
extern "C" volatile float g_tex_s_scale[2]  = {1.0f, 1.0f};
extern "C" volatile float g_tex_t_scale[2]  = {1.0f, 1.0f};
extern "C" volatile float g_tex_s_offset[2] = {0.0f, 0.0f};
extern "C" volatile float g_tex_t_offset[2] = {0.0f, 0.0f};
extern "C" volatile uint8_t g_es1_alpha_test_enable = 0;
extern "C" volatile float   g_es1_alpha_test_ref    = 0.0f;
extern "C" volatile uint8_t g_es1_highp_alpha       = 0;
extern "C" volatile uint8_t g_es1_tex0_in_rgb       = 1;
extern "C" volatile uint8_t g_es1_front_face_cw     = 0;
extern "C" volatile uint8_t g_es1_force_2d          = 0;
extern "C" volatile uint8_t g_es1_depth_clamp_active = 0;
extern "C" volatile uint8_t g_es1_base_modulate     = 1;
extern "C" volatile uint8_t g_es1_base_color_mode   = 0;
extern "C" volatile uint8_t g_es1_prim_rgba[4]      = {255,255,255,255};
extern "C" volatile uint8_t g_es1_env_rgba[4]       = {255,255,255,255};
extern "C" volatile uint8_t g_es1_use_tex0          = 1;
extern "C" volatile uint8_t g_es1_use_tex1          = 0;



static void gfx_opengl_start_frame(void) {}

static void gfx_opengl_finish_render(void) {}

static void gfx_opengl_on_resize(void) {}

static const char* gfx_opengl_get_name(void) { return "OpenGL 1.1"; }

static int gfx_opengl_get_max_texture_size(void) {
#if defined(__PSP__)
    // PSP GU supports 512x512 textures; some wrappers may not report this reliably.
    GLint size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &size);
    if (size <= 0) size = 512;
    if (size < 512) size = 512;
    return (int)size;
#else
    GLint size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &size);
    if (size <= 0) size = 512;
    return size;
#endif
}

static struct GfxClipParameters gfx_opengl_get_clip_parameters(void) {
    return (struct GfxClipParameters){ false, false };
}

// --- Minimal framebuffer emulation for GLES 1.1 ---
// We do not rely on FBOs (which are not core in GLES 1.1). Instead, we:
// - Represent each framebuffer as a GL texture (color only)
// - Copy from the default framebuffer into that texture via glReadPixels
// - When asked to copy from a framebuffer to the main buffer, draw a screen‑aligned textured quad
// This is sufficient for Perfect Dark's framebuffer effects (pause blur, lens, etc.).

static void ensure_fb_index(int fb_id) {
    if (fb_id < 0) fb_id = 0;
    if ((int)s_fbs.size() <= fb_id) s_fbs.resize(fb_id + 1);
}

static void allocate_fb_texture(GLESFramebuffer &fb) {
    if (!fb.allocated) {
        glGenTextures(1, &fb.tex);
        fb.allocated = true;
    }
    glBindTexture(GL_TEXTURE_2D, fb.tex);
    s_last_bound_tex = fb.tex;
    // Default sampler suitable for UI copies
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, current_filter_mode == FILTER_LINEAR ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, current_filter_mode == FILTER_LINEAR ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Allocate backing storage
#if defined(__PSP__)
    const uint32_t max_tex = (uint32_t)gfx_opengl_get_max_texture_size();
    if (fb.w > max_tex) fb.w = max_tex;
    if (fb.h > max_tex) fb.h = max_tex;
    auto next_pot = [](uint32_t v) -> uint32_t { uint32_t p = 1; while (p < v) p <<= 1; return p; };
    fb.pot_w = next_pot(fb.w ? fb.w : 1);
    fb.pot_h = next_pot(fb.h ? fb.h : 1);
    if (fb.pot_w > max_tex) fb.pot_w = max_tex;
    if (fb.pot_h > max_tex) fb.pot_h = max_tex;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)fb.pot_w, (GLsizei)fb.pot_h, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, NULL);
#endif
}

static void fb_copy_window_into_texture(GLESFramebuffer &dst, int src_x0, int src_y0, int src_w, int src_h, bool use_back) {
    if (!dst.allocated || dst.tex == 0 || dst.w == 0 || dst.h == 0) return;
    glBindTexture(GL_TEXTURE_2D, dst.tex);
    s_last_bound_tex = dst.tex;
#ifndef __PSP__
    // Prefer GPU-side copy on non-PSP targets
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, (GLsizei)dst.w, (GLsizei)dst.h);
    dst.valid = true;
#else
    // PSP: read the current draw buffer from the PSPGL surface and upload.
    pspgl_surface *surf = reinterpret_cast<pspgl_surface*>(surface);
    if (!surf) {
        dst.valid = false;
        return;
    }

    pspgl_buffer *buf = use_back ? surf->draw : surf->display;
    if (!buf || !buf->base) {
        dst.valid = false;
        return;
    }

    // Ensure the draw buffer contents are complete before CPU reads.
    if (use_back) {
        glFinish();
    }

    const uint16_t *src565 = static_cast<const uint16_t*>(buf->base);
    const int win_w = (int)(gfx_current_dimensions.width ? gfx_current_dimensions.width : (uint32_t)SCREEN_WIDTH);
    const int win_h = (int)(gfx_current_dimensions.height ? gfx_current_dimensions.height : (uint32_t)SCREEN_HEIGHT);
    const uint32_t stride = surf->stride ? (uint32_t)surf->stride : (uint32_t)win_w;

    int sx0 = src_x0;
    int sy0 = src_y0;
    int sx1 = src_x0 + src_w;
    int sy1 = src_y0 + src_h;
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    if (sx1 > win_w) sx1 = win_w;
    if (sy1 > win_h) sy1 = win_h;
    const int region_w_i = sx1 - sx0;
    const int region_h_i = sy1 - sy0;
    if (region_w_i <= 0 || region_h_i <= 0) {
        dst.valid = false;
        return;
    }

    const uint32_t region_w = (uint32_t)region_w_i;
    const uint32_t region_h = (uint32_t)region_h_i;
    const uint32_t copy_w = dst.w;
    const uint32_t copy_h = dst.h;
    if (copy_w == 0 || copy_h == 0) {
        dst.valid = false;
        return;
    }

    static std::vector<uint16_t> psp_bb_tmp;
    const size_t needed = (size_t)copy_w * (size_t)copy_h;
    if (psp_bb_tmp.size() < needed) psp_bb_tmp.resize(needed);

    const uint32_t factor_x = (copy_w != 0 && (region_w % copy_w) == 0) ? (region_w / copy_w) : 0;
    const uint32_t factor_y = (copy_h != 0 && (region_h % copy_h) == 0) ? (region_h / copy_h) : 0;

    if (factor_x >= 1 && factor_y >= 1) {
        const uint32_t samples = factor_x * factor_y;
        for (uint32_t y = 0; y < copy_h; ++y) {
            uint16_t *drow = psp_bb_tmp.data() + (size_t)y * (size_t)copy_w;
            const uint32_t src_y0 = (uint32_t)sy0 + y * factor_y;
            for (uint32_t x = 0; x < copy_w; ++x) {
                const uint32_t src_x0 = (uint32_t)sx0 + x * factor_x;
                uint32_t sum_r = 0, sum_g = 0, sum_b = 0;
                for (uint32_t sy = 0; sy < factor_y; ++sy) {
                    const uint16_t *srow = src565 + (src_y0 + sy) * stride + src_x0;
                    for (uint32_t sx = 0; sx < factor_x; ++sx) {
                        const uint16_t c = srow[sx];
                        // PSP draw/display buffers are in BGR565 (GU_PSM_5650) layout.
                        // Convert to RGBA4444 for GL texture upload.
                        sum_r += c & 0x1f;
                        sum_g += (c >> 5) & 0x3f;
                        sum_b += (c >> 11) & 0x1f;
                    }
                }
                const uint16_t r = (uint16_t)(sum_r / samples);
                const uint16_t g = (uint16_t)(sum_g / samples);
                const uint16_t b = (uint16_t)(sum_b / samples);
                drow[x] = (uint16_t)(((r >> 1) << 12) | ((g >> 2) << 8) | ((b >> 1) << 4) | 0x000f);
            }
        }
    } else {
        // Fallback: nearest sampling (non-integer scale factors)
        const float step_x = (float)region_w / (float)copy_w;
        const float step_y = (float)region_h / (float)copy_h;

        for (uint32_t y = 0; y < copy_h; ++y) {
            const uint32_t src_y = (uint32_t)sy0 + std::min<uint32_t>(region_h - 1, (uint32_t)((y + 0.5f) * step_y));
            const uint16_t *srow = src565 + src_y * stride;
            uint16_t *drow = psp_bb_tmp.data() + (size_t)y * (size_t)copy_w;
            for (uint32_t x = 0; x < copy_w; ++x) {
                const uint32_t src_x = (uint32_t)sx0 + std::min<uint32_t>(region_w - 1, (uint32_t)((x + 0.5f) * step_x));
                const uint16_t c = srow[src_x];
                const uint16_t r = c & 0x1f;
                const uint16_t g = (c >> 5) & 0x3f;
                const uint16_t b = (c >> 11) & 0x1f;
                drow[x] = (uint16_t)(((r >> 1) << 12) | ((g >> 2) << 8) | ((b >> 1) << 4) | 0x000f);
            }
        }
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)copy_w, (GLsizei)copy_h, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, psp_bb_tmp.data());
    dst.valid = true;
#endif
}

static void fb_draw_textured_quad(GLuint tex, float x, float y, float w, float h, bool invert_v, bool opaque_replace) {
    // Render a simple textured quad in screen space (origin top-left)
    begin_2d_batch();

    glEnable(GL_TEXTURE_2D);
    glDisable(GL_CULL_FACE);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    if (opaque_replace) {
        glDisable(GL_BLEND);
    } else {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }

    glBindTexture(GL_TEXTURE_2D, tex);

    const GLfloat x0 = (GLfloat)x;
    const GLfloat y0 = (GLfloat)y;
    const GLfloat x1 = (GLfloat)(x + w);
    const GLfloat y1 = (GLfloat)(y + h);

    const GLfloat verts[4 * 3] = {
        x0, y0, 0.0f,
        x1, y0, 0.0f,
        x0, y1, 0.0f,
        x1, y1, 0.0f,
    };

    // On PSP when using POT textures, only sample the valid sub-rect
    GLfloat s_max = 1.0f, t_max = 1.0f;
#if defined(__PSP__)
    for (const auto &fb : s_fbs) {
        if (fb.allocated && fb.tex == tex) {
            const float pw = (float)(fb.pot_w ? fb.pot_w : fb.w);
            const float ph = (float)(fb.pot_h ? fb.pot_h : fb.h);
            if (pw > 0.0f) s_max = (GLfloat)((float)fb.w / pw);
            if (ph > 0.0f) t_max = (GLfloat)((float)fb.h / ph);
            break;
        }
    }
#endif
    const GLfloat t0 = invert_v ? t_max : 0.0f;
    const GLfloat t1 = invert_v ? 0.0f : t_max;
    const GLfloat uvs[4 * 2] = {
        0.0f, t0,
        s_max, t0,
        0.0f, t1,
        s_max, t1,
    };

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, verts);
    glTexCoordPointer(2, GL_FLOAT, 0, uvs);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);

    end_2d_batch();
}

void* gfx_opengl_get_framebuffer_texture_id(int fb_id) {
    if (fb_id <= 0) return NULL;
    ensure_fb_index(fb_id);
    const GLESFramebuffer &fb = s_fbs[fb_id];
    if (!fb.allocated || fb.tex == 0) return NULL;
    // Cast integer id to pointer-sized value expected by the caller
    return (void*)(uintptr_t)fb.tex;
}

void gfx_opengl_clear_framebuffer(bool c, bool d) {
    // Always clear - we're always rendering to the window in this fallback implementation
    GLbitfield mask = 0;
    if (c) mask |= GL_COLOR_BUFFER_BIT;
    if (d) { glDepthMask(GL_TRUE); mask |= GL_DEPTH_BUFFER_BIT; }
    if (mask) glClear(mask);
    glDepthMask(current_depth_mask ? GL_TRUE : GL_FALSE);
}

void gfx_opengl_copy_framebuffer(int fb_dst, int fb_src, int l, int t, bool flip_y, bool use_back) {
    // Three main cases we care about:
    // 1) src = 0 (main), dst > 0: copy the window into a texture
    // 2) src > 0, dst = 0: draw the framebuffer texture to the window
    // 3) src > 0, dst > 0: slow path – draw to window, then copy to dst

    // Ensure entries exist
    ensure_fb_index(fb_src);
    ensure_fb_index(fb_dst);

    if (fb_src == 0 && fb_dst > 0) {
        // Copy the window buffer into the destination texture.
        GLESFramebuffer &dst = s_fbs[fb_dst];
        if (!dst.allocated || dst.tex == 0 || dst.w == 0 || dst.h == 0) return;
        (void)flip_y;

        const uint32_t native_w_u = gfx_current_native_viewport.width ? gfx_current_native_viewport.width : 1;
        const uint32_t native_h_u = gfx_current_native_viewport.height ? gfx_current_native_viewport.height : 1;
        const uint32_t win_w_u = gfx_current_dimensions.width ? gfx_current_dimensions.width : (uint32_t)SCREEN_WIDTH;
        const uint32_t win_h_u = gfx_current_dimensions.height ? gfx_current_dimensions.height : (uint32_t)SCREEN_HEIGHT;

        const bool want_full_viewport =
            (l < 0 || t < 0) ||
            (l == 0 && t == 0 && dst.w == native_w_u && dst.h == native_h_u);

        if (want_full_viewport) {
            fb_copy_window_into_texture(dst, 0, 0, (int)win_w_u, (int)win_h_u, use_back);
        } else {
            const float scale_x = (float)win_w_u / (float)native_w_u;
            const float scale_y = (float)win_h_u / (float)native_h_u;
            const int src_x0 = (int)floorf((float)l * scale_x);
            const int src_y0 = (int)floorf((float)t * scale_y);
            const int src_w = (int)ceilf((float)dst.w * scale_x);
            const int src_h = (int)ceilf((float)dst.h * scale_y);
            fb_copy_window_into_texture(dst, src_x0, src_y0, src_w, src_h, use_back);
        }
        return;
    }

    if (fb_src > 0 && fb_dst == 0) {
        // Draw the framebuffer texture to the window.
        const GLESFramebuffer &src = s_fbs[fb_src];
        if (!src.allocated || src.tex == 0 || src.w == 0 || src.h == 0) return;
        const float x = (float)l;
        const float y = (float)t;
        // Copy semantics: replace destination pixels in target region
        fb_draw_textured_quad(src.tex, x, y, (float)src.w, (float)src.h, src.invert_y, true);
        return;
    }

    if (fb_src > 0 && fb_dst > 0) {
        // Offscreen-to-offscreen copy not supported without FBOs.
        // Avoid drawing to the window to prevent artifacts.
        return;
    }

    // fb_src == 0 && fb_dst == 0 -> nothing to do
}

void gfx_opengl_resolve_msaa_color_buffer(int fb_id_target, int fb_id_source) {
    // No MSAA resolve in GLES1 fallback – noop
    (void)fb_id_target; (void)fb_id_source;
}

bool gfx_opengl_start_draw_to_framebuffer(int fb_id, float noise_scale) {
    // We cannot redirect rendering in GLES1 without FBOs; just remember the target.
    // fb_id == 0 means draw to default window; non-zero means the game intends offscreen.
    (void)noise_scale;
    s_current_draw_fb = fb_id;
    return true;
}

int gfx_opengl_create_framebuffer(void) {
    // Allocate a new framebuffer entry and return its id
    int id = (int)s_fbs.size();
    s_fbs.resize(id + 1);
    // Texture will be created on first parameter update
    if (s_system_game_fb_primary < 0) {
        // Heuristic: first created FB is the game’s primary offscreen
        s_system_game_fb_primary = id;
    }
    return id;
}

void gfx_opengl_update_framebuffer_parameters(int fb, uint32_t w, uint32_t h, uint32_t msaa, bool inv_y, bool rt, bool d, bool extract) {
    (void)msaa; (void)rt; (void)d; (void)extract;
    if (fb <= 0) return; // main/window fb is implicit
    ensure_fb_index(fb);
    GLESFramebuffer &dst = s_fbs[fb];
    if (dst.w == w && dst.h == h && dst.allocated) {
        dst.invert_y = inv_y;
        return;
    }
    dst.w = w; dst.h = h; dst.invert_y = inv_y;
    allocate_fb_texture(dst);
    const uint32_t alloc_w = (dst.pot_w ? dst.pot_w : (dst.w ? dst.w : 1));
    const uint32_t alloc_h = (dst.pot_h ? dst.pot_h : (dst.h ? dst.h : 1));
    // Allocate storage initialized to black; prefer 16-bit RGBA to save memory
    {
        const size_t px = (size_t)alloc_w * (size_t)alloc_h;
        static std::vector<uint16_t> zeros;
        if (zeros.size() < px) zeros.assign(px, 0x0000);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        psp_clear_gl_errors();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)alloc_w, (GLsizei)alloc_h, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, zeros.data());
        if (psp_check_gl_error("glTexImage2D framebuffer alloc", (int)alloc_w, (int)alloc_h, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4) != GL_NO_ERROR) {
            dst.valid = false;
            return;
        }
    }
    dst.valid = false;
}

void gfx_opengl_select_texture_fb(int fb_id) {
    if (fb_id <= 0) { glBindTexture(GL_TEXTURE_2D, 0); s_tex_id[0] = 0; s_last_bound_tex = 0; return; }
    ensure_fb_index(fb_id);
    const GLESFramebuffer &src = s_fbs[fb_id];
    if (!src.allocated || src.tex == 0) { glBindTexture(GL_TEXTURE_2D, 0); s_tex_id[0] = 0; s_last_bound_tex = 0; return; }
    glBindTexture(GL_TEXTURE_2D, src.tex);
    // Ensure subsequent draws using TEXEL0 pick this texture
    s_tex_id[0] = src.tex;
    s_last_bound_tex = src.tex;
}

void gfx_opengl_set_texture_filter(FilteringMode mode) { current_filter_mode = mode; }
FilteringMode gfx_opengl_get_texture_filter(void) { return current_filter_mode; }

struct GfxRenderingAPI gfx_opengl_api = {
    gfx_opengl_get_name,
    gfx_opengl_get_max_texture_size,
    gfx_opengl_get_clip_parameters,
    gfx_opengl_unload_shader,
    gfx_opengl_load_shader,
    gfx_opengl_create_and_load_new_shader,
    gfx_opengl_lookup_shader,
    gfx_opengl_shader_get_info,
    gfx_opengl_clear_shaders,
    (uint32_t (*)())gfx_opengl_new_texture,
    (void (*)(int, uint32_t, bool))gfx_opengl_select_texture,
    gfx_opengl_upload_texture,
    gfx_opengl_set_sampler_parameters,
    gfx_opengl_set_depth_mode,
    gfx_opengl_set_depth_range,
    gfx_opengl_set_viewport,
    gfx_opengl_set_scissor,
    gfx_opengl_set_use_alpha,
    gfx_opengl_draw_triangles,
    gfx_opengl_init,
    gfx_opengl_on_resize,
    gfx_opengl_start_frame,
    gfx_opengl_end_frame,
    gfx_opengl_finish_render,
    gfx_opengl_create_framebuffer,
    gfx_opengl_update_framebuffer_parameters,
    gfx_opengl_start_draw_to_framebuffer,
    gfx_opengl_copy_framebuffer,
    gfx_opengl_clear_framebuffer,
    gfx_opengl_resolve_msaa_color_buffer,
    gfx_opengl_get_framebuffer_texture_id,
    gfx_opengl_select_texture_fb,
    gfx_opengl_delete_texture,
    gfx_opengl_set_texture_filter,
    gfx_opengl_get_texture_filter
};
