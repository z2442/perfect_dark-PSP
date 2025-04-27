#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <SDL.h>
#include <GL/gl.h>
#include <PR/gbi.h>
#ifndef G_TX_CLAMP
#define G_TX_CLAMP 0x02
#endif
#ifndef G_TX_MIRROR
#define G_TX_MIRROR 0x01
#endif
#include "gfx_rendering_api.h"

#define SCREEN_WIDTH  480  // Set the screen width (example value)
#define SCREEN_HEIGHT 272  // Set the screen height (example value)

struct LoadedVertex {
    float x, y, z, w;   // Position
    float u, v;          // Texture coordinates
    uint8_t r, g, b, a;  // Color (RGBA)
};

static float P_matrix[4][4]; // Global matrix for projection

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

/* -- GLES 1.1 depth‑state shadow ---------------------------------------- */
static bool es_depth_test  = false; /* GL_DEPTH_TEST currently enabled? */
static bool es_depth_write = true;  /* TRUE if glDepthMask(GL_TRUE)      */

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
    /* OpenGL 1.1 / OpenGL ES 1.1 require POT textures. */
    uint32_t pot_w = 1, pot_h = 1;
    while (pot_w < width)  pot_w <<= 1;
    while (pot_h < height) pot_h <<= 1;

    const bool needs_resize = (pot_w != width) || (pot_h != height);

    /* Prepare a POT‑sized temporary buffer if necessary. */
    const uint8_t* src = rgba32_buf;
    std::vector<uint8_t> temp;              /* RAII helper */
    if (needs_resize) {
        temp.resize(pot_w * pot_h * 4, 0);  /* zero‑fill padding */

        for (uint32_t y = 0; y < height; ++y) {
            memcpy(&temp[(y * pot_w) * 4],
                   &rgba32_buf[(y * width) * 4],
                   width * 4);
        }
        src = temp.data();
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);          /* allow arbitrary row‑length */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 pot_w, pot_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, src);

    /* Default filters/wrap will be overridden by set_sampler_parameters(). */
}

static void gfx_opengl_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    GLint filter = linear_filter ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    GLint wrap_s;
    if (cms & G_TX_MIRROR) {
        wrap_s = GL_REPEAT;                /* MIRRORED_REPEAT not in GL 1.1 */
    } else {
        wrap_s = (cms & G_TX_CLAMP) ? GL_CLAMP : GL_REPEAT;
    }

    GLint wrap_t;
    if (cmt & G_TX_MIRROR) {
        wrap_t = GL_REPEAT;
    } else {
        wrap_t = (cmt & G_TX_CLAMP) ? GL_CLAMP : GL_REPEAT;
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_s);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_t);
}

/* N64  ➜  GLES 1.1 depth-state conversion
 *  enable  == “run the depth test?”
 *  write   == “update the depth buffer?”
 *  compare == “reject/accept based on depth?” (else always pass)
 *  zmode   : 0 Opaque, 1 InterPen, 2 Translucent, 3 Decal
 */
static void gfx_opengl_set_depth_mode(bool   enable,
    bool   write,
    bool   compare,
    bool   /*depth_source_prim*/,
    uint16_t zmode)
{
/* ------------------------------------------------------------------ */
/* Enable / disable GL_DEPTH_TEST                                     */
/* ------------------------------------------------------------------ */
if (enable != es_depth_test) {
if (enable) glEnable(GL_DEPTH_TEST);
else        glDisable(GL_DEPTH_TEST);
es_depth_test = enable;
}

/* ------------------------------------------------------------------ */
/* Depth-buffer writes (glDepthMask)                                   */
/* Core flag ‘write’ is TRUE when we *want* to write.                  */
/* ------------------------------------------------------------------ */
if (write != es_depth_write) {
glDepthMask(write ? GL_TRUE : GL_FALSE);
es_depth_write = write;
}

/* If the test is disabled we’re done. */
if (!enable)
return;

/* ------------------------------------------------------------------ */
/* Choose a depth-compare function                                     */
/* N64 works in 1/w screen-space; “<” ≈ GL_GEQUAL/GL_LEQUAL.           */
/* ------------------------------------------------------------------ */
GLenum func = GL_ALWAYS;            /* default when ‘compare’ == false */

if (compare) {
switch (zmode & 3) {
case 0:  /* ZMODE_OPA   */ func = GL_LEQUAL;  break;
case 1:  /* ZMODE_INTER */ func = GL_LESS;    break;
case 2:  /* ZMODE_XLU   */ func = GL_LEQUAL;  break;
case 3:  /* ZMODE_DECAL */ func = GL_ALWAYS;  break; /* disable compare */
default:                 func = GL_LEQUAL;  break;
}
}

glDepthFunc(func);
}


/* Add perspective projection for 3D content */
static void gfx_opengl_set_perspective_projection(float fov, float aspect_ratio, float near_clip, float far_clip) {
    float top = tanf(fov * 0.5f) * near_clip;
    float right = top * aspect_ratio;

    P_matrix[0][0] = near_clip / right;
    P_matrix[1][1] = near_clip / top;
    P_matrix[2][2] = -(far_clip + near_clip) / (far_clip - near_clip);
    P_matrix[2][3] = -(2 * far_clip * near_clip) / (far_clip - near_clip);
    P_matrix[3][2] = -1.0f;
    P_matrix[3][3] = 0.0f;
}

/* Add orthographic projection for 2D content */
static void gfx_opengl_set_orthographic_projection(float left, float right, float bottom, float top, float near_clip, float far_clip) {
    P_matrix[0][0] = 2.0f / (right - left);
    P_matrix[1][1] = 2.0f / (top - bottom);
    P_matrix[2][2] = -2.0f / (far_clip - near_clip);
    P_matrix[3][0] = -(right + left) / (right - left);
    P_matrix[3][1] = -(top + bottom) / (top - bottom);
    P_matrix[3][2] = -(far_clip + near_clip) / (far_clip - near_clip);
    P_matrix[3][3] = 1.0f;
}

static float gfx_adjust_x_for_aspect_ratio(float x) {
    // This function can be modified to adjust x based on your aspect ratio needs
    float aspect_ratio = (float)SCREEN_WIDTH / (float)SCREEN_HEIGHT;
    return x * aspect_ratio;
}

static void gfx_opengl_set_projection_for_2d() {
    gfx_opengl_set_orthographic_projection(0, SCREEN_WIDTH, 0, SCREEN_HEIGHT, -1.0f, 1.0f);
    glDisable(GL_DEPTH_TEST);  // Disable depth testing for 2D rendering

    glViewport(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);  // Set the viewport to the entire screen for 2D rendering
    glScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);   // Ensure 2D content is clipped to the screen boundaries
    glEnable(GL_SCISSOR_TEST);
}

static void gfx_opengl_set_projection_for_3d() {
    gfx_opengl_set_perspective_projection(60.0f, (float)SCREEN_WIDTH / (float)SCREEN_HEIGHT, 0.1f, 1000.0f);
    glEnable(GL_DEPTH_TEST);  // Enable depth testing for 3D rendering

    glViewport(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);  // Set the viewport to the entire screen for 3D rendering
    glDisable(GL_SCISSOR_TEST);  // Disable scissor test for 3D content, usually not necessary
}

static void gfx_opengl_set_depth_range(float znear, float zfar) {
    glDepthRange(znear, zfar);
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
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
}

static bool is_2d_mode = false;

static void gfx_opengl_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    const int stride_floats = 9; // not 8
    const int stride_bytes = stride_floats * sizeof(float);

    if (!buf_vbo || buf_vbo_num_tris == 0) return;

    bool is_2d = true;

    // Loop through the vertex buffer and inspect each vertex
    for (size_t i = 0; i < buf_vbo_len; i += stride_bytes) {  // 9 floats per vertex (3 for position, 2 for texture, 4 for color)
        struct LoadedVertex* vertex = reinterpret_cast<LoadedVertex*>(&buf_vbo[i]);

        // Check if the vertex has a Z value (typically used for 3D objects)
        if (vertex->z == 0.0f && vertex->w == 1.0f) {
            is_2d = true;  // If z == 0, it's likely a 2D object
            break;
        }
    }

    // Set projection based on whether it's 2D or 3D
    if (is_2d) {
        if (!is_2d_mode) {
            is_2d_mode = true;
            gfx_opengl_set_projection_for_2d();  // Switch to 2D projection matrix
        }
    } else {
        if (is_2d_mode) {
            is_2d_mode = false;
            gfx_opengl_set_projection_for_3d();  // Switch to 3D projection matrix
        }
    }

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_FLOAT, stride_bytes, buf_vbo + 5);

    glVertexPointer(3, GL_FLOAT, stride_bytes, buf_vbo);
    glTexCoordPointer(2, GL_FLOAT, stride_bytes, buf_vbo + 3);

    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    glDrawArrays(GL_TRIANGLES, 0, buf_vbo_num_tris * 3);

    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);

    glPopMatrix();
}

static void gfx_opengl_init(void) {
    
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthFunc(GL_LEQUAL);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glClearColor(0.0, 0.0, 0.0, 1.0);
}


/* Update texture coordinate adjustments */
static void gfx_apply_texture_coordinates(LoadedVertex* vertex, const float tex_scaling[2], const int16_t uls, const int16_t ult, const int16_t width, const int16_t height) {
    // Apply texture scaling
    float u = vertex->u / width;
    float v = vertex->v / height;

    // Adjust for aspect ratio if necessary
    u = gfx_adjust_x_for_aspect_ratio(u);

    // Map the texels to the appropriate space based on the texture parameters
    vertex->u = (uls + u * tex_scaling[0]);
    vertex->v = (ult + v * tex_scaling[1]);
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