#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <SDL.h>
#include <GLES/gl.h>
#include <PR/gbi.h>
#include "gfx_rendering_api.h"

#define SCREEN_WIDTH  480  // Set the screen width (example value)
#define SCREEN_HEIGHT 272  // Set the screen height (example value)

static uint32_t cms = 0;
static uint32_t cmt = 0;

static uint16_t g_es_zmode = 0;

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
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);          /* allow arbitrary row‑length */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba32_buf);
}

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    // Only GL_REPEAT and GL_CLAMP_TO_EDGE are supported in GLES 1.1.
    if (val & G_TX_CLAMP)
        return GL_CLAMP_TO_EDGE;
    // MIRROR is NOT supported; we'll emulate it in software.
    return GL_REPEAT;
}

static float mirror_coord(float uv) {
    // uv: input texture coordinate (0..N)
    int ipart = (int)floorf(uv);
    float fpart = uv - ipart;
    // Flip on every odd tile
    if (ipart & 1)
        return 1.0f - fpart;
    else
        return fpart;
}

static uint32_t current_cms = 0;
static uint32_t current_cmt = 0;

static void gfx_opengl_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms_in, uint32_t cmt_in) {
    // Save for later (software mirroring)
    current_cms = cms_in;
    current_cmt = cmt_in;

    const GLint filter = linear_filter && (current_filter_mode == FILTER_LINEAR) ? GL_LINEAR : GL_NEAREST;

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

    // GLES 1.1 supports only GL_REPEAT and GL_CLAMP_TO_EDGE
    GLint wrap_s = (cms_in & G_TX_CLAMP) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
    GLint wrap_t = (cmt_in & G_TX_CLAMP) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_s);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_t);
}

/* N64  ➜  GLES 1.1 depth-state conversion
 *  enable  == “run the depth test?”
 *  write   == “update the depth buffer?”
 *  compare == “reject/accept based on depth?” (else always pass)
 *  zmode   : 0 Opaque, 1 InterPen, 2 Translucent, 3 Decal
 */
static float g_polygon_offset_z = 0.0f;
static void gfx_opengl_set_depth_mode(
    bool enable,
    bool write,
    bool compare,
    bool depth_source_prim,
    uint16_t zmode
) {
    // Enable/disable depth test
    if (enable != es_depth_test) {
        if (enable) glEnable(GL_DEPTH_TEST);
        else        glDisable(GL_DEPTH_TEST);
        es_depth_test = enable;
    }

    // Enable/disable depth buffer writes
    if (write != es_depth_write) {
        glDepthMask(write ? GL_TRUE : GL_FALSE);
        es_depth_write = write;
    }

    if (!enable) {
        g_es_zmode = zmode;
        return;
    }

    // Set depth comparison function
    GLenum func = GL_ALWAYS;
    if (compare) {
        func = GL_LEQUAL; // N64 mostly uses LEQUAL for everything
    }
    glDepthFunc(func);

    // GLES 1.1 cannot do polygon offset for triangles—fudge Z in software
    switch (zmode & 3) {
        case 1: // InterPen
            g_polygon_offset_z = +0.0005f;
            break;
        case 3: // Decal
            g_polygon_offset_z = -0.0005f;
            break;
        default:
            g_polygon_offset_z = 0.0f;
            break;
    }

    g_es_zmode = zmode;
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

    if (!buf_vbo || buf_vbo_num_tris == 0) return;

    // Figure out fudge factor for Z, only for decal/interpen
    float z_fudge = 0.0f;
    int zmode = g_es_zmode;
    switch (zmode & 3) {
        case 1: // InterPen
            z_fudge = +0.0005f;
            break;
        case 3: // Decal
            z_fudge = -0.0005f;
            break;
        default:
            z_fudge = 0.0f;
            break;
    }

    bool is_2d = true;

    // Loop through the vertex buffer and inspect each vertex
    for (size_t i = 0; i < buf_vbo_len; i += stride_bytes) {  // 9 floats per vertex (3 for position, 2 for texture, 4 for color)
        struct LoadedVertex* vertex = reinterpret_cast<LoadedVertex*>(&buf_vbo[i]);

        // Check if the vertex has a Z value (typically used for 3D objects)
        if (vertex->z == 0.0f && vertex->w == 1.0f) {
            is_2d = true;  // If z == 0, it's likely a 2D object
            break;
        }

        if (cms & G_TX_MIRROR) vertex->u = mirror_coord(vertex->u);
        if (cmt & G_TX_MIRROR) vertex->v = mirror_coord(vertex->v);

        vertex->z += g_polygon_offset_z;
    }

    // Set projection based on whether it's 2D or 3D
    

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

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