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
#include <SDL.h>
#include <GLES/gl.h>


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

#define SCREEN_WIDTH  480  // Set the screen width (example value)
#define SCREEN_HEIGHT 272  // Set the screen height (example value)

static bool es_depth_test  = false; /* GL_DEPTH_TEST currently enabled? */
static bool es_depth_write = false;  /* TRUE if glDepthMask(GL_TRUE)      */

static float P_matrix[4][4]; // Global matrix for projection

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
};

static std::vector<GLESFramebuffer> s_fbs; // index 0 is the default (window) fb

// --- 2D batch helpers: isolate state so 3D path stays untouched ---
static void begin_2d_batch() {
    // Save PROJECTION and set screen-space ortho
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrthof(0.0f, (GLfloat)SCREEN_WIDTH, (GLfloat)SCREEN_HEIGHT, 0.0f, -1.0f, 1.0f);

    // Save MODELVIEW and reset
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    // Disable depth test for pure 2D; remember we shadow the intended state in es_depth_*.
    if (es_depth_test) glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
}

static void end_2d_batch() {
    // Restore MODELVIEW then PROJECTION
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();

    // Restore depth state per shadow flags so 3D resumes exactly as before
    if (es_depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(es_depth_write ? GL_TRUE : GL_FALSE);

    // Return to MODELVIEW for client-state draws
    glMatrixMode(GL_MODELVIEW);
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

static void glLoadRowMajorMatrixf(const float m[4][4]) {
    // Our matrices are laid out the way OpenGL expects already; load directly.
    glLoadMatrixf(&m[0][0]);
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
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    // Screen-space ortho: origin top-left, +x right, +y down
    glOrthof(0.0f, (GLfloat)SCREEN_WIDTH, (GLfloat)SCREEN_HEIGHT, 0.0f, -1.0f, 1.0f);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void gfx_opengl_set_projection_for_3d() {
    // Load external matrices provided by the engine
    glMatrixMode(GL_PROJECTION);
    glLoadRowMajorMatrixf(g_es1_P);
    glMatrixMode(GL_MODELVIEW);
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

static FilteringMode current_filter_mode = FILTER_LINEAR;
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
    current_textures_linear_filter[tile] = linear_filter;
}

static void gfx_opengl_upload_texture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height) {
    const size_t num_pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (g_es1_highp_alpha) {
        if (s_has_texenv_combine) {
            // Premultiplied RGBA8888 for high-precision alpha (fonts/UI)
            static std::vector<uint8_t> pma;
            pma.resize(num_pixels * 4);
            const uint8_t* src = rgba32_buf;
            uint8_t*       dst = pma.data();
            size_t i = 0;
            const size_t n4 = num_pixels & ~static_cast<size_t>(3);
            for (; i < n4; i += 4) {
                uint8_t r0 = src[0],  g0 = src[1],  b0 = src[2],  a0 = src[3];
                uint8_t r1 = src[4],  g1 = src[5],  b1 = src[6],  a1 = src[7];
                uint8_t r2 = src[8],  g2 = src[9],  b2 = src[10], a2 = src[11];
                uint8_t r3 = src[12], g3 = src[13], b3 = src[14], a3 = src[15];
                src += 16;
                r0 = (uint8_t)((r0 * a0 + 127) / 255); g0 = (uint8_t)((g0 * a0 + 127) / 255); b0 = (uint8_t)((b0 * a0 + 127) / 255);
                r1 = (uint8_t)((r1 * a1 + 127) / 255); g1 = (uint8_t)((g1 * a1 + 127) / 255); b1 = (uint8_t)((b1 * a1 + 127) / 255);
                r2 = (uint8_t)((r2 * a2 + 127) / 255); g2 = (uint8_t)((g2 * a2 + 127) / 255); b2 = (uint8_t)((b2 * a2 + 127) / 255);
                r3 = (uint8_t)((r3 * a3 + 127) / 255); g3 = (uint8_t)((g3 * a3 + 127) / 255); b3 = (uint8_t)((b3 * a3 + 127) / 255);
                dst[0]  = r0; dst[1]  = g0; dst[2]  = b0; dst[3]  = a0;
                dst[4]  = r1; dst[5]  = g1; dst[6]  = b1; dst[7]  = a1;
                dst[8]  = r2; dst[9]  = g2; dst[10] = b2; dst[11] = a2;
                dst[12] = r3; dst[13] = g3; dst[14] = b3; dst[15] = a3;
                dst += 16;
            }
            for (; i < num_pixels; ++i) {
                uint8_t r = src[0], g = src[1], b = src[2], a = src[3];
                src += 4;
                r = (uint8_t)((r * a + 127) / 255);
                g = (uint8_t)((g * a + 127) / 255);
                b = (uint8_t)((b * a + 127) / 255);
                *dst++ = r; *dst++ = g; *dst++ = b; *dst++ = a;
            }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                         width, height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE,
                         pma.data());
        } else {
            // No combine available: keep RGB as white, carry alpha only.
            // This lets GL_MODULATE produce color=PRIMARY and alpha=TEX.alpha.
            static std::vector<uint8_t> alpha_rgba;
            alpha_rgba.resize(num_pixels * 4);
            const uint8_t* src = rgba32_buf;
            uint8_t*       dst = alpha_rgba.data();
            size_t i = 0;
            const size_t n4 = num_pixels & ~static_cast<size_t>(3);
            for (; i < n4; i += 4) {
                uint8_t a0 = src[3];
                uint8_t a1 = src[7];
                uint8_t a2 = src[11];
                uint8_t a3 = src[15];
                src += 16;
                dst[0]  = 255; dst[1]  = 255; dst[2]  = 255; dst[3]  = a0;
                dst[4]  = 255; dst[5]  = 255; dst[6]  = 255; dst[7]  = a1;
                dst[8]  = 255; dst[9]  = 255; dst[10] = 255; dst[11] = a2;
                dst[12] = 255; dst[13] = 255; dst[14] = 255; dst[15] = a3;
                dst += 16;
            }
            for (; i < num_pixels; ++i) {
                uint8_t a = src[3];
                src += 4;
                *dst++ = 255; *dst++ = 255; *dst++ = 255; *dst++ = a;
            }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                         width, height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE,
                         alpha_rgba.data());
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
            uint8_t r0 = src[0],  g0 = src[1],  b0 = src[2],  a0 = src[3];
            uint8_t r1 = src[4],  g1 = src[5],  b1 = src[6],  a1 = src[7];
            uint8_t r2 = src[8],  g2 = src[9],  b2 = src[10], a2 = src[11];
            uint8_t r3 = src[12], g3 = src[13], b3 = src[14], a3 = src[15];
            src += 16;
            r0 = (uint8_t)((r0 * a0 + 127) / 255); g0 = (uint8_t)((g0 * a0 + 127) / 255); b0 = (uint8_t)((b0 * a0 + 127) / 255);
            r1 = (uint8_t)((r1 * a1 + 127) / 255); g1 = (uint8_t)((g1 * a1 + 127) / 255); b1 = (uint8_t)((b1 * a1 + 127) / 255);
            r2 = (uint8_t)((r2 * a2 + 127) / 255); g2 = (uint8_t)((g2 * a2 + 127) / 255); b2 = (uint8_t)((b2 * a2 + 127) / 255);
            r3 = (uint8_t)((r3 * a3 + 127) / 255); g3 = (uint8_t)((g3 * a3 + 127) / 255); b3 = (uint8_t)((b3 * a3 + 127) / 255);
            dst[0] = (uint16_t)(((r0 >> 4) << 12) | ((g0 >> 4) << 8) | ((b0 >> 4) << 4) | (a0 >> 4));
            dst[1] = (uint16_t)(((r1 >> 4) << 12) | ((g1 >> 4) << 8) | ((b1 >> 4) << 4) | (a1 >> 4));
            dst[2] = (uint16_t)(((r2 >> 4) << 12) | ((g2 >> 4) << 8) | ((b2 >> 4) << 4) | (a2 >> 4));
            dst[3] = (uint16_t)(((r3 >> 4) << 12) | ((g3 >> 4) << 8) | ((b3 >> 4) << 4) | (a3 >> 4));
            dst += 4;
        }
        for (; i < num_pixels; ++i) {
            uint8_t r = src[0], g = src[1], b = src[2], a = src[3];
            src += 4;
            r = (uint8_t)((r * a + 127) / 255);
            g = (uint8_t)((g * a + 127) / 255);
            b = (uint8_t)((b * a + 127) / 255);
            *dst++ = (uint16_t)(((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4));
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     width, height, 0,
                     GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4,
                     rgba16.data());
    }
}

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    switch (val) {
        case G_TX_NOMIRROR | G_TX_CLAMP:
            return GL_CLAMP_TO_EDGE;

        case G_TX_NOMIRROR | G_TX_WRAP:
            return GL_REPEAT;

        case G_TX_MIRROR | G_TX_WRAP:
            return GL_REPEAT;


        case G_TX_MIRROR | G_TX_CLAMP:
#ifdef GL_MIRROR_CLAMP_TO_EDGE_ATI
            return GL_MIRROR_CLAMP_TO_EDGE_ATI;
#elif defined(GL_MIRROR_CLAMP_ATI)
            return GL_MIRROR_CLAMP_ATI;
#else
            return GL_CLAMP_TO_EDGE; // Fallback
#endif
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
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glScalef(g_tex_s_scale[tile], g_tex_t_scale[tile], 1.0f);
    glTranslatef(g_tex_s_offset[tile], g_tex_t_offset[tile], 0.0f);
    glMatrixMode(GL_MODELVIEW);
}


static void gfx_opengl_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    // Suppress draws when targeting custom offscreen FBOs we cannot render to.
    // Allow draws when targeting the system primary offscreen (the game’s main render target),
    // because that path clears the screen before presenting the resolved texture.
    if (s_current_draw_fb != 0 && s_current_draw_fb != s_system_game_fb_primary) {
        return; // pretend we drew offscreen; texture content comes from copy ops
    }

    const int stride_floats = 9; // pos(3) + uv(2) + color(4)
    const int stride_bytes = stride_floats * sizeof(float);

    // Install P/M or temporary ortho for forced 2D batches
    bool forced2D = (g_es1_force_2d != 0);
    bool prevDepthTestLocal = es_depth_test;
    bool prevDepthMaskLocal = current_depth_mask;
    if (forced2D) {
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        // NDC ortho to match N64-style UI math (-1..1)
        glOrthof(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        // Disable depth test and writes for UI
        if (prevDepthTestLocal) glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        // 2D shouldn't be culled
        glDisable(GL_CULL_FACE);
    } else {
        if (g_es1_matrix_dirty) {
            glMatrixMode(GL_PROJECTION);
            glLoadRowMajorMatrixf(g_es1_P);
            glMatrixMode(GL_MODELVIEW);
            glLoadRowMajorMatrixf(g_es1_M);
            g_es1_matrix_dirty = 0;
        }
    }

    // Depth test state is managed by set_depth_mode; do not toggle here.

    // Keep CCW winding; CPU flips per-limb if mirrored.
    glFrontFace(GL_CCW);
    // Apply N64 cull mode (was disabled above for forced 2D)
    if (!forced2D && g_es1_cull_mode == 0) {
        glDisable(GL_CULL_FACE);
    } else if (!forced2D) {
        glEnable(GL_CULL_FACE);
        glCullFace(g_es1_cull_mode == 1 ? GL_BACK : GL_FRONT);
    }

    glMatrixMode(GL_MODELVIEW);

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
        // If TEXEL0 is a framebuffer texture with inverted V, apply via texture matrix
        bool needs_invert_v = false;
        for (const auto &fb : s_fbs) {
            if (fb.allocated && fb.tex == s_tex_id[0] && fb.invert_y) { needs_invert_v = true; break; }
        }
        if (needs_invert_v) {
            glMatrixMode(GL_TEXTURE);
            glLoadIdentity();
            glScalef(1.0f, -1.0f, 1.0f);
            glTranslatef(0.0f, -1.0f, 0.0f);
            glMatrixMode(GL_MODELVIEW);
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
                    set_texenv_texture_modulate_with_constant(true);
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
    bool using_two_pass = (g_force_two_pass != 0) && g_es1_use_tex1 && (s_tex_id[1] != 0) && !(g_es1_highp_alpha && g_es1_alpha_test_enable);
    if (using_two_pass) {
        glDisable(GL_BLEND);
    } else {
        // For fonts/UI, prefer alpha-test only without blending for speed and crisp edges
        if (g_es1_highp_alpha && g_es1_alpha_test_enable) {
            glDisable(GL_BLEND);
        } else if (prevBlend) {
            glEnable(GL_BLEND);
            // Ensure blend func matches the last requested mode
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
        const bool want_alpha_test = g_es1_alpha_test_enable || (prevBlend && !s_last_modulate);
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

    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
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
        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
        // Restore depth
        if (prevDepthTestLocal) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        glDepthMask(prevDepthMaskLocal ? GL_TRUE : GL_FALSE);
    }
    // Depth test restored by set_depth_mode elsewhere
}

static void gfx_opengl_init(void) {
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    // Default to PMA-friendly blending; set_use_alpha will override as needed
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    // Default to CCW so front faces aren't culled
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glCullFace(GL_BACK);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

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
    }

    // Set state trackers to match initial GL state
    s_last_use_alpha = true;
    s_last_modulate  = false;
    s_current_depth_func = GL_LEQUAL;
    s_texenv_mode = TEXENV_UNKNOWN;
}
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
extern "C" volatile uint8_t g_es1_base_modulate     = 1;
extern "C" volatile uint8_t g_es1_base_color_mode   = 0;
extern "C" volatile uint8_t g_es1_prim_rgba[4]      = {255,255,255,255};
extern "C" volatile uint8_t g_es1_env_rgba[4]       = {255,255,255,255};
extern "C" volatile uint8_t g_es1_use_tex0          = 1;
extern "C" volatile uint8_t g_es1_use_tex1          = 0;



static void gfx_opengl_start_frame(void) {}
static void gfx_opengl_end_frame(void) { glFlush(); }
static void gfx_opengl_finish_render(void) {}

static void gfx_opengl_on_resize(void) {}

static const char* gfx_opengl_get_name(void) { return "OpenGL 1.1"; }

static int gfx_opengl_get_max_texture_size(void) {
    GLint size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &size);
    return size;
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
    // Default sampler suitable for UI copies
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, current_filter_mode == FILTER_LINEAR ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, current_filter_mode == FILTER_LINEAR ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void fb_copy_window_into_texture(GLESFramebuffer &dst) {
    if (!dst.allocated || dst.tex == 0 || dst.w == 0 || dst.h == 0) return;
    // Read back from the default framebuffer and upload to texture
    static std::vector<uint8_t> s_readback;
    const size_t need = (size_t)dst.w * (size_t)dst.h * 4u;
    if (s_readback.size() < need) s_readback.resize(need);

    // Read lower-left origin; orientation will be handled when drawing (invert_v)
    glReadPixels(0, 0, (GLsizei)dst.w, (GLsizei)dst.h, GL_RGBA, GL_UNSIGNED_BYTE, s_readback.data());
    // Ensure opaque alpha; default framebuffer may not carry meaningful alpha
    for (size_t i = 0; i < need; i += 4) s_readback[i + 3] = 255;

    glBindTexture(GL_TEXTURE_2D, dst.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)dst.w, (GLsizei)dst.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, s_readback.data());
    dst.valid = true;
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
        x1, y1, 0.0f,
        x0, y1, 0.0f,
    };

    const GLfloat t0 = invert_v ? 1.0f : 0.0f;
    const GLfloat t1 = invert_v ? 0.0f : 1.0f;
    const GLfloat uvs[4 * 2] = {
        0.0f, t0,
        1.0f, t0,
        1.0f, t1,
        0.0f, t1,
    };

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, verts);
    glTexCoordPointer(2, GL_FLOAT, 0, uvs);

    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

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
    // Avoid clearing the visible buffer if we are supposedly drawing to a custom offscreen FB
    if (s_current_draw_fb != 0 && s_current_draw_fb != s_system_game_fb_primary) {
        return; // no-op for offscreen clears in fallback
    }
    GLbitfield mask = 0;
    if (c) mask |= GL_COLOR_BUFFER_BIT;
    if (d) { glDepthMask(GL_TRUE); mask |= GL_DEPTH_BUFFER_BIT; }
    glClear(mask);
    glDepthMask(current_depth_mask);
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
        // Copy the current window backbuffer into the destination texture
        GLESFramebuffer &dst = s_fbs[fb_dst];
        if (!dst.allocated || dst.tex == 0 || dst.w == 0 || dst.h == 0) return;
        // Copy full region; l/t are ignored for full-screen effects
        fb_copy_window_into_texture(dst);
        return;
    }

    if (fb_src > 0 && fb_dst == 0) {
        // Only present to the window when we're drawing to the main buffer
        // and the source is the primary game framebuffer.
        if (s_current_draw_fb == 0 && fb_src == s_system_game_fb_primary) {
            const GLESFramebuffer &src = s_fbs[fb_src];
            if (!src.allocated || src.tex == 0 || src.w == 0 || src.h == 0) return;
            const float x = (float)l;
            const float y = (float)t;
            // Copy semantics: replace destination pixels in target region
            fb_draw_textured_quad(src.tex, x, y, (float)src.w, (float)src.h, src.invert_y, true);
        }
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
    // Allocate storage initialized to black; prefer 16-bit RGBA to save memory
    {
        const size_t px = (size_t)w * (size_t)h;
        static std::vector<uint16_t> zeros;
        if (zeros.size() < px) zeros.assign(px, 0x0000);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, zeros.data());
    }
    dst.valid = false;
}

void gfx_opengl_select_texture_fb(int fb_id) {
    if (fb_id <= 0) { glBindTexture(GL_TEXTURE_2D, 0); s_tex_id[0] = 0; return; }
    ensure_fb_index(fb_id);
    const GLESFramebuffer &src = s_fbs[fb_id];
    if (!src.allocated || src.tex == 0) { glBindTexture(GL_TEXTURE_2D, 0); s_tex_id[0] = 0; return; }
    glBindTexture(GL_TEXTURE_2D, src.tex);
    // Ensure subsequent draws using TEXEL0 pick this texture
    s_tex_id[0] = src.tex;
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
