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

#include <PR/gbi.h>
#include "gfx_rendering_api.h"

#define SCREEN_WIDTH  480  // Set the screen width (example value)
#define SCREEN_HEIGHT 272  // Set the screen height (example value)

static bool es_depth_test  = false; /* GL_DEPTH_TEST currently enabled? */
static bool es_depth_write = false;  /* TRUE if glDepthMask(GL_TRUE)      */

static float P_matrix[4][4]; // Global matrix for projection

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

static uint32_t cms = 0;
static uint32_t cmt = 0;

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
    glBindTexture(GL_TEXTURE_2D, texture_id);
    current_textures_linear_filter[tile] = linear_filter;
}

static void gfx_opengl_upload_texture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height) {
    /* Convert incoming 32‑bit RGBA8888 to 16‑bit RGBA4444 to cut texture memory
       footprint in half.  GLES 1.x will store the texture exactly as supplied
       when we pass type = GL_UNSIGNED_SHORT_4_4_4_4. */

    const size_t num_pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    static std::vector<uint16_t> rgba16;
    rgba16.resize(num_pixels);

    const uint8_t* src = rgba32_buf;
    uint16_t*       dst = rgba16.data();

    // Light unroll for better throughput on PSP's in-order core.
    size_t i = 0;
    const size_t n4 = num_pixels & ~static_cast<size_t>(3);
    for (; i < n4; i += 4) {
        uint8_t r0 = src[0], g0 = src[1], b0 = src[2], a0 = src[3];
        uint8_t r1 = src[4], g1 = src[5], b1 = src[6], a1 = src[7];
        uint8_t r2 = src[8], g2 = src[9], b2 = src[10], a2 = src[11];
        uint8_t r3 = src[12],g3 = src[13],b3 = src[14],a3 = src[15];
        src += 16;
        dst[0] = static_cast<uint16_t>(((r0 >> 4) << 12) | ((g0 >> 4) << 8) | ((b0 >> 4) << 4) | (a0 >> 4));
        dst[1] = static_cast<uint16_t>(((r1 >> 4) << 12) | ((g1 >> 4) << 8) | ((b1 >> 4) << 4) | (a1 >> 4));
        dst[2] = static_cast<uint16_t>(((r2 >> 4) << 12) | ((g2 >> 4) << 8) | ((b2 >> 4) << 4) | (a2 >> 4));
        dst[3] = static_cast<uint16_t>(((r3 >> 4) << 12) | ((g3 >> 4) << 8) | ((b3 >> 4) << 4) | (a3 >> 4));
        dst += 4;
    }
    for (; i < num_pixels; ++i) {
        uint8_t r = src[0], g = src[1], b = src[2], a = src[3];
        src += 4;
        *dst++ = static_cast<uint16_t>(((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4));
    }

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,          /* internalformat */
                 width, height, 0,
                 GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, /* format/type */
                 rgba16.data());
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
    ::cms = cms;
    ::cmt = cmt;

    const GLint filter = (linear_filter && current_filter_mode == FILTER_LINEAR) ? GL_LINEAR : GL_NEAREST;

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

    GLenum wrap_s = gfx_cm_to_opengl(cms);
    GLenum wrap_t = gfx_cm_to_opengl(cmt);
    if (wrap_s == GL_MIRRORED_REPEAT) wrap_s = GL_REPEAT;
    if (wrap_t == GL_MIRRORED_REPEAT) wrap_t = GL_REPEAT;

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
                    break;
                case ZMODE_OPA:
                case ZMODE_XLU:
                    if (depth_source_prim) {
                        glDepthFunc(GL_LEQUAL);
                    } else {
                        glDepthFunc(GL_LESS);
                    }
                    break;
                case ZMODE_DEC:
                    glDepthFunc(GL_LEQUAL);
                    // Polygon offset is not available in GLES 1.1; can't mimic exactly.
                    break;
            }
        } else {
            glDepthFunc(GL_ALWAYS);
        }
    } else {
        glDisable(GL_DEPTH_TEST);
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
    if (use_alpha) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
    if (modulate) {
        glBlendFunc(GL_DST_COLOR, GL_ZERO);
    } else {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
}


static void gfx_opengl_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    const int stride_floats = 9; // not 8
    const int stride_bytes = stride_floats * sizeof(float);

    // Detect 2D by counting how many verts are effectively z≈0 and measure z range
    const size_t total_verts = buf_vbo_num_tris * 3;
    size_t near_zero_z = 0;
    float minz =  1e30f, maxz = -1e30f;
    const float Z_EPS = 1e-3f;   // tolerate tiny numerical noise in z
    for (size_t v = 0; v < total_verts; ++v) {
        float* base = buf_vbo + v * stride_floats; // [x,y,z,u,v,r,g,b,a]
        const float z = base[2];
        if (fabsf(z) < Z_EPS) ++near_zero_z;
        if (z < minz) minz = z; if (z > maxz) maxz = z;
    }
    const float z_span = maxz - minz;

    // Inspect XY range to decide if 2D vertices are in screen space (pixels) or NDC [-1..1]
    float minx =  1e30f, miny =  1e30f;
    float maxx = -1e30f, maxy = -1e30f;
    for (size_t v = 0; v < total_verts; ++v) {
        float* base = buf_vbo + v * stride_floats;
        float x = base[0], y = base[1];
        if (x < minx) minx = x; if (x > maxx) maxx = x;
        if (y < miny) miny = y; if (y > maxy) maxy = y;
    }
    const float maxAbsXY = fmaxf(fabsf(maxx), fmaxf(fabsf(maxy), fmaxf(fabsf(minx), fabsf(miny))));
    const bool ndc_like = (maxAbsXY <= 1.2f);   // vertices look like NDC [-1..1]

    // Decide if this draw is truly 2D.
    // Add planar-z requirement and hard exclusion for obvious 3D spans, while keeping tolerance for UI overscan.
    const bool in_screen_pixels = (minx >= -8.0f && maxx <= (float)SCREEN_WIDTH + 8.0f &&
                                   miny >= -8.0f && maxy <= (float)SCREEN_HEIGHT + 8.0f);
    const bool z_mostly_zero = (near_zero_z * 20 >= total_verts * 19); // >=95%
    const bool z_planar = (z_span < 5e-3f); // all tris lie on (almost) one plane

    // Exclude obvious 3D: wide z range while depth writes are enabled or NDC not used
    const bool obviously_3d = (z_span > 1e-2f) && es_depth_write;

    bool batch_is_2d = (!obviously_3d) &&
                       (!es_depth_write) &&
                       (ndc_like || in_screen_pixels) &&
                       (z_mostly_zero || z_planar);

    // Apply software mirroring to UVs in-place if requested by N64 wrap flags
    for (size_t v = 0; v < total_verts; ++v) {
        float* base = buf_vbo + v * stride_floats; // [x,y,z, u,v, r,g,b,a]
        if (cms & G_TX_MIRROR) base[3] = mirror_coord(base[3]);
        if (cmt & G_TX_MIRROR) base[4] = mirror_coord(base[4]);
    }

    // Decide path and install matrices explicitly per batch
    const bool do_2d = batch_is_2d;
    if (do_2d) {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        if (ndc_like) {
            // NDC-like coordinates; Y-up
            glOrthof(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);
        } else {
            // Pixel coordinates; origin bottom-left, Y-up
            glOrthof(0.0f, (GLfloat)SCREEN_WIDTH, 0.0f, (GLfloat)SCREEN_HEIGHT, -1.0f, 1.0f);
        }
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glDisable(GL_CULL_FACE);
    } else {
        // 3D: always load engine-provided matrices to avoid stale state
        glMatrixMode(GL_PROJECTION);
        glLoadRowMajorMatrixf(g_es1_P);
        glMatrixMode(GL_MODELVIEW);
        glLoadRowMajorMatrixf(g_es1_M);
        g_es1_matrix_dirty = 0;
    }

    // For 2D, temporarily disable depth test for the draw only, then restore
    const bool prev_depth = es_depth_test;
    if (do_2d && prev_depth) glDisable(GL_DEPTH_TEST);

    // Keep CCW winding; CPU flips per-limb if mirrored.
    glFrontFace(GL_CCW);
    if (do_2d) {
        // 2D should never be culled
        glDisable(GL_CULL_FACE);
    } else {
        // Apply N64 cull mode only for 3D
        if (g_es1_cull_mode == 0) {
            glDisable(GL_CULL_FACE);
        } else {
            glEnable(GL_CULL_FACE);
            glCullFace(g_es1_cull_mode == 1 ? GL_BACK : GL_FRONT);
        }
    }

    glMatrixMode(GL_MODELVIEW);

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_FLOAT, stride_bytes, buf_vbo + 5);

    glVertexPointer(3, GL_FLOAT, stride_bytes, buf_vbo);
    glTexCoordPointer(2, GL_FLOAT, stride_bytes, buf_vbo + 3);

    glEnable(GL_TEXTURE_2D);
    
    glDrawArrays(GL_TRIANGLES, 0, buf_vbo_num_tris * 3);

    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);

    // Restore depth test if we disabled it for 2D
    if (do_2d && prev_depth) glEnable(GL_DEPTH_TEST);
}

static void gfx_opengl_init(void) {
    
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Default to CCW so front faces aren't culled
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glCullFace(GL_BACK);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glClearColor(0.0, 0.0, 0.0, 1.0);
}



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

void* gfx_opengl_get_framebuffer_texture_id(int fb_id) { return NULL; }

void gfx_opengl_clear_framebuffer(bool c, bool d) {
    GLbitfield mask = 0;
    if (c) mask |= GL_COLOR_BUFFER_BIT;
    if (d) { glDepthMask(GL_TRUE); mask |= GL_DEPTH_BUFFER_BIT; }
    glClear(mask);
    glDepthMask(current_depth_mask);
}

void gfx_opengl_copy_framebuffer(int fb_dst, int fb_src, int l, int t, bool flip_y, bool use_back) {}
void gfx_opengl_resolve_msaa_color_buffer(int fb_id_target, int fb_id_source) {}
bool gfx_opengl_start_draw_to_framebuffer(int fb_id, float noise_scale) { return false; }
int gfx_opengl_create_framebuffer(void) { return 0; }
void gfx_opengl_update_framebuffer_parameters(int fb, uint32_t w, uint32_t h, uint32_t msaa, bool inv_y, bool rt, bool d, bool extract) {}
void gfx_opengl_select_texture_fb(int fb_id) {}

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