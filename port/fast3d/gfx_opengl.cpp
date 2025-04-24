#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#include <pspgu.h>
#include <pspgum.h>
#include <pspdisplay.h>
#include <pspge.h>

static unsigned int __attribute__((aligned(16))) list[262144];

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
    // GU does not use texture IDs, return a dummy handle
    static GLuint dummy_tex_id = 1;
    return dummy_tex_id++;
}

static void gfx_opengl_delete_texture(uint32_t texID) {
    // No-op for GU; textures are not reference counted or deleted by ID
    (void)texID;
}

static void gfx_opengl_select_texture(int tile, GLuint texture_id, bool linear_filter) {
    // No-op for GU; texture is already bound via sceGuTexImage
    current_textures_linear_filter[tile] = linear_filter;
}

static void gfx_opengl_upload_texture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height) {
    // Find next power-of-two width and height
    uint32_t pot_width = 1;
    uint32_t pot_height = 1;
    while (pot_width < width) pot_width <<= 1;
    while (pot_height < height) pot_height <<= 1;

    // Allocate and convert to ABGR (PSP expects this format)
    uint32_t* converted = (uint32_t*)sceGuGetMemory(pot_width * pot_height * sizeof(uint32_t));
    for (uint32_t y = 0; y < pot_height; ++y) {
        for (uint32_t x = 0; x < pot_width; ++x) {
            if (x < width && y < height) {
                uint32_t rgba = ((const uint32_t*)rgba32_buf)[y * width + x];
                uint8_t r = (rgba >> 0) & 0xFF;
                uint8_t g = (rgba >> 8) & 0xFF;
                uint8_t b = (rgba >> 16) & 0xFF;
                uint8_t a = (rgba >> 24) & 0xFF;
                converted[y * pot_width + x] = (a << 24) | (b << 16) | (g << 8) | r;
            } else {
                converted[y * pot_width + x] = 0;
            }
        }
    }

    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, pot_width, pot_height, pot_width, converted);
}

static void gfx_opengl_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    sceGuTexFilter(linear_filter ? GU_LINEAR : GU_NEAREST, linear_filter ? GU_LINEAR : GU_NEAREST);

    int wrap_s = (cms & G_TX_CLAMP) ? GU_CLAMP : GU_REPEAT;
    int wrap_t = (cmt & G_TX_CLAMP) ? GU_CLAMP : GU_REPEAT;

    sceGuTexWrap(wrap_s, wrap_t);
}

static void gfx_opengl_set_depth_mode(bool depth_test, bool depth_update, bool depth_compare, bool depth_source_prim, uint16_t zmode) {
    if (depth_test) {
        sceGuEnable(GU_DEPTH_TEST);
        sceGuDepthMask(!depth_update);
        sceGuDepthFunc(depth_compare ? GU_GEQUAL : GU_ALWAYS);
    } else {
        sceGuDisable(GU_DEPTH_TEST);
    }
    current_depth_mask = depth_update;
}

static void gfx_opengl_set_depth_range(float znear, float zfar) {
    uint16_t near_val = (uint16_t)(65535.0f * (1.0f - znear));
    uint16_t far_val  = (uint16_t)(65535.0f * (1.0f - zfar));
    sceGuDepthRange(near_val, far_val);
}

static void gfx_opengl_set_viewport(int x, int y, int width, int height) {
    sceGuViewport(2048 + x, 2048 + y, width, height);
}

static void gfx_opengl_set_scissor(int x, int y, int width, int height) {
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuScissor(x, y, x + width, y + height);
}

static void gfx_opengl_set_use_alpha(bool use_alpha, bool modulate) {
    if (use_alpha) {
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    } else {
        sceGuDisable(GU_BLEND);
    }
}

static void gfx_opengl_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (!buf_vbo || buf_vbo_num_tris == 0) return;

    typedef struct {
        float u, v;
        unsigned int color;
        float x, y, z;
    } Vertex;

    const size_t num_verts = buf_vbo_num_tris * 3;
    Vertex* verts = (Vertex*)sceGuGetMemory(sizeof(Vertex) * num_verts);

    for (size_t i = 0; i < num_verts; ++i) {
        float* src = &buf_vbo[i * 9];
        verts[i].x = src[0];
        verts[i].y = src[1];
        verts[i].z = src[2];
        verts[i].u = src[3];
        verts[i].v = src[4];
        float r = src[5], g = src[6], b = src[7], a = src[8];
        verts[i].color =
            ((uint8_t)(a * 255.0f) << 24) |
            ((uint8_t)(b * 255.0f) << 16) |
            ((uint8_t)(g * 255.0f) << 8)  |
            ((uint8_t)(r * 255.0f));
    }

    sceGuEnable(GU_TEXTURE_2D);
    sceGuShadeModel(GU_SMOOTH);
    sceGuDisable(GU_LIGHTING);

    sceGuDrawArray(GU_TRIANGLES,
        GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF,
        num_verts, 0, verts);
}

static void gfx_opengl_init(void) {
    void* fbp0 = guGetStaticVramBuffer(480, 272, GU_PSM_8888);
    void* fbp1 = guGetStaticVramBuffer(480, 272, GU_PSM_8888);
    void* zbp  = guGetStaticVramBuffer(480, 272, GU_PSM_4444);

    sceGuInit();
    sceGuStart(GU_DIRECT, list);

    sceGuDrawBuffer(GU_PSM_8888, fbp0, 512);
    sceGuDispBuffer(480, 272, fbp1, 512);
    sceGuDepthBuffer(zbp, 512);
    sceGuOffset(2048 - (480 / 2), 2048 - (272 / 2));
    sceGuViewport(2048, 2048, 480, 272);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, 480, 272);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuFrontFace(GU_CW);
    sceGuShadeModel(GU_SMOOTH);
    sceGuDisable(GU_TEXTURE_2D);

    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

static void gfx_opengl_start_frame(void) {
    sceGuStart(GU_DIRECT, list);

    float fov = 70.0f;
    float aspect = 480.0f / 272.0f;
    float znear = 0.1f;
    float zfar = 100.0f;
    float f = 1.0f / tanf(fov * (3.1415926f / 360.0f));

    ScePspFMatrix4 projection = {
        {f / aspect, 0.0f, 0.0f, 0.0f},
        {0.0f, f, 0.0f, 0.0f},
        {0.0f, 0.0f, zfar / (zfar - znear), 1.0f},
        {0.0f, 0.0f, (-znear * zfar) / (zfar - znear), 0.0f}
    };

    ScePspFMatrix4 identity = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f}
    };

    // Use sceGuSetMatrix for setting GU matrices (sceGuLoadMatrix is not allowed for inline matrix setup)
    sceGuSetMatrix(GU_PROJECTION, &projection);
    sceGuSetMatrix(GU_VIEW, &identity);
    sceGuSetMatrix(GU_MODEL, &identity);
}
static void gfx_opengl_end_frame(void) {
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}
static void gfx_opengl_finish_render(void) {
    // No-op for GU
}

static void gfx_opengl_on_resize(void) {
    // Not applicable for PSP GU
}

static const char* gfx_opengl_get_name(void) {
    return "PSP GU";
}

static int gfx_opengl_get_max_texture_size(void) {
    return 512;
}

static struct GfxClipParameters gfx_opengl_get_clip_parameters(void) {
    return (struct GfxClipParameters){ false, false };
}

void* gfx_opengl_get_framebuffer_texture_id(int fb_id) {
    return NULL;
}

void gfx_opengl_clear_framebuffer(bool c, bool d) {
    unsigned int flags = 0;
    if (c) flags |= GU_COLOR_BUFFER_BIT;
    if (d) flags |= GU_DEPTH_BUFFER_BIT;
    sceGuClear(flags);
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