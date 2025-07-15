#include "pspdebug.h"
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
    GLuint gu_program_id;
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

static void gfx_gu_unload_shader(struct ShaderProgram* old_prg) {}

static void gfx_gu_load_shader(struct ShaderProgram* new_prg) {}

static struct ShaderProgram* gfx_gu_create_and_load_new_shader(uint64_t shader_id0, uint32_t shader_id1) {
    static struct ShaderProgram dummy;
    memset(&dummy, 0, sizeof(dummy));
    dummy.used_textures[0] = true;
    return &dummy;
}

static struct ShaderProgram* gfx_gu_lookup_shader(uint64_t shader_id0, uint32_t shader_id1) {
    return gfx_gu_create_and_load_new_shader(shader_id0, shader_id1);
}

static void gfx_gu_shader_get_info(struct ShaderProgram* prg, uint8_t* num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static void gfx_gu_clear_shaders(void) {}

// max number of textures you’ll ever allocate
#define MAX_TEXTURES 256

/*
 * These next few functions are shims to replace the standard opengl 
 * functions that normally would interact with a "texture" cache.
 *
 * Through some digging it seems like the basic flow uploading/using a texture is 
 * as follows:
 *
 * 1. Call new_texture() to generate a texture spot in the texture cache
 * 2. Call select_texture() to bind a texture, also selects that texture spot as the 
 *    'acting' texture so that the next call can populate it 
 * 3. Call upload_texture() to write the texture data to bound texture's buffer
 * 4. Call draw_triangles() to draw triangles with whatever the last selected texture was
 */
// Storing the important information about a texture
typedef struct {
    GLuint  name;
    uint8_t*   handle; 
    bool    in_use;
    uint32_t pot_width;
    uint32_t pot_height;
} TextureSlot;

// Static texture storage
static TextureSlot texture_slots[MAX_TEXTURES] = {0};
// Used to hold the next available texture id
static GLuint      next_id = 1;

static GLuint SelectedTexture = 1;

// Generate texture name
void guGenTexture(GLuint *texture_id) {
    GLuint name = next_id++;
//     *texture_id = name;
    
    // find a free slot and reserve it
    for (int i = 0; i < MAX_TEXTURES; i++) {
        if (!texture_slots[i].in_use) {
            texture_slots[i].in_use = true;
            texture_slots[i].name   = name;
            texture_slots[i].handle = NULL;  
            *texture_id = i;
            break;
        }
    }
}

// This will free the texture data associated with the texture_id and open it for use
void guDeleteTexture(GLuint texture_id) {
    GLuint name = texture_id;
    for (int i = 0; i < MAX_TEXTURES; i++) {
        if (texture_slots[i].in_use && texture_slots[i].name == name) {
            if (texture_slots[i].handle)
                free(texture_slots[i].handle);
            texture_slots[i].in_use = false;
            return;
        }
    }
}

void guBindTexture(GLuint texture_id) {
    for (int i = 0; i < MAX_TEXTURES; i++) {
        if (texture_slots[i].in_use && texture_slots[i].name == texture_id) {
            SelectedTexture = texture_id;
            
            sceGuEnable(GU_TEXTURE_2D);
            sceGuTexMode(GU_PSM_8888, 0, 0, 0);
            sceGuTexImage(0, 
                          texture_slots[texture_id].pot_width, 
                          texture_slots[texture_id].pot_height, 
                          texture_slots[texture_id].pot_width, 
                          texture_slots[texture_id].handle
                          );
            return;
        }
    };
}
static GLuint gfx_gu_new_texture(void) {
    GLuint tex;
    guGenTexture(&tex);
    return tex;
}

static void gfx_gu_delete_texture(uint32_t texID) {
    GLuint gl_tex_id = (GLuint)texID;
    guDeleteTexture(gl_tex_id);
}

static void gfx_gu_select_texture(int tile, GLuint texture_id, bool linear_filter) {
    guBindTexture(texture_id);
    current_textures_linear_filter[tile] = linear_filter;
}

static void gfx_gu_upload_texture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height) {
    // If texture hasn't been created, return
    if (!texture_slots[SelectedTexture].in_use) return;
    
    int pot_width = 1, pot_height = 1;
    // Calculate nearest power of two width and height for texture
    while (pot_width < (int)width)   pot_width <<= 1;
    while (pot_height < (int)height) pot_height <<= 1;

    // Allocate RAM buffer for converted texture
    uint8_t* converted = (uint8_t*)malloc(pot_width * pot_height * 4);
 
    if (!converted) {
        pspDebugScreenPrintf("OUT OF RAM");
        return; // Out of RAM
    }

    // Copy the data into the new resized texture cache
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* dst_row = converted + y * pot_width * 4;
        const uint8_t* src_row = rgba32_buf + y * width     * 4;
        memcpy(dst_row, src_row, width * 4);
        memset(dst_row + width * 4,
               0,
               (pot_width - width) * 4);
    }

    
    texture_slots[SelectedTexture].handle = converted;
    texture_slots[SelectedTexture].pot_width = pot_width;
    texture_slots[SelectedTexture].pot_height = pot_height;
}

static void gfx_gu_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    sceGuTexFilter(linear_filter ? GU_LINEAR : GU_NEAREST, linear_filter ? GU_LINEAR : GU_NEAREST);

    int wrap_s = (cms & G_TX_CLAMP) ? GU_CLAMP : GU_REPEAT;
    int wrap_t = (cmt & G_TX_CLAMP) ? GU_CLAMP : GU_REPEAT;

    sceGuTexWrap(wrap_s, wrap_t);
}

static void gfx_gu_set_depth_mode(bool depth_test, bool depth_update, bool depth_compare, bool depth_source_prim, uint16_t zmode) {
    if (depth_test) {
        sceGuEnable(GU_DEPTH_TEST);
        sceGuDepthMask(!depth_update);
        sceGuDepthFunc(depth_compare ? GU_GEQUAL : GU_ALWAYS);
    } else {
        sceGuDisable(GU_DEPTH_TEST);
    }
    current_depth_mask = depth_update;
}

static void gfx_gu_set_depth_range(float znear, float zfar) {
    uint16_t near_val = (uint16_t)(65535.0f * (1.0f - znear));
    uint16_t far_val  = (uint16_t)(65535.0f * (1.0f - zfar));
    sceGuDepthRange(near_val, far_val);
}

static void gfx_gu_set_viewport(int x, int y, int width, int height) {
    sceGuViewport(2048 + x, 2048 + y, width, height);
}

static void gfx_gu_set_scissor(int x, int y, int width, int height) {
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuScissor(x, y, x + width, y + height);
}

static void gfx_gu_set_use_alpha(bool use_alpha, bool modulate) {
    if (use_alpha) {
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    } else {
        sceGuDisable(GU_BLEND);
    }

    if (modulate){
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    } else {
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    }
}

static void gfx_gu_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (!buf_vbo || buf_vbo_num_tris == 0) return;

    sceGuEnable(GU_TEXTURE_2D);
    sceGuShadeModel(GU_SMOOTH);
    sceGuDisable(GU_LIGHTING);
//     sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
//     sceGuTexFilter(GU_LINEAR, GU_LINEAR);
//     sceGumMatrixMode(GU_PROJECTION);
//     sceGumPerspective(90.0, 16.0 / 9.0, 0.50, 40.0);
    // Setup matrices for rendering
//     sceGumMatrixMode(GU_MODEL);
//     sceGumLoadIdentity();
    
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
    
    
    sceGuDrawArray(GU_TRIANGLES,
        GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF,
        num_verts, 0, verts);
}

static void gfx_gu_init(void) {
    // Allocate framebuffers in VRAM (align width to 512 for PSP)
    void* fbp0 = guGetStaticVramBuffer(512, 272, GU_PSM_8888);
    void* fbp1 = guGetStaticVramBuffer(512, 272, GU_PSM_8888);
    void* zbp  = guGetStaticVramBuffer(512, 272, GU_PSM_4444);

    sceGuInit();
    sceGuStart(GU_DIRECT, list);

    // Set up double buffering
    sceGuDrawBuffer(GU_PSM_8888, fbp0, 512);
    sceGuDispBuffer(480, 272, fbp1, 512);
    sceGuDepthBuffer(zbp, 512);

    // Set screen offset and viewport to center
    sceGuOffset(2048 - (480 / 2), 2048 - (272 / 2));
    sceGuViewport(2048, 2048, 480, 272);
    
    // Fov, Aspect Ratio, Near clipping field, far clipping field
    sceGumPerspective(90.0, 16.0 / 9.0, 0.50, 40.0);

    // Set depth range (1.0 near, 0.0 far in GU, which is 65535 to 0)
    sceGuDepthRange(65535, 0);
    
    // Enable scissor test to restrict drawing to visible region
    sceGuScissor(0, 0, 480, 272);
    sceGuEnable(GU_SCISSOR_TEST);

    // Set correct face winding (clockwise for GU, matching gu default)
//     sceGuEnable(GU_CULL_FACE);
    sceGuFrontFace(GU_CW);

    // Enable smooth shading
    sceGuShadeModel(GU_SMOOTH);

    // Start with textures disabled; will be enabled by draw routines as needed
    sceGuDisable(GU_TEXTURE_2D);

    // Clear color and depth buffers on first frame
    sceGuClearColor(0); // Black background
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

static void gfx_gu_start_frame(void) {
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
    
    sceGuSetMatrix(GU_PROJECTION, &projection);
    sceGuSetMatrix(GU_VIEW, &identity);
    sceGuSetMatrix(GU_MODEL, &identity);
}

static void gfx_gu_end_frame(void) {
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

static void gfx_gu_finish_render(void) {
    // No-op for GU
}

static void gfx_gu_on_resize(void) {
    // Not applicable for PSP GU
}

static const char* gfx_gu_get_name(void) {
    return "PSP GU";
}

static int gfx_gu_get_max_texture_size(void) {
    return 512;
}

static struct GfxClipParameters gfx_gu_get_clip_parameters(void) {
    return (struct GfxClipParameters){ false, false };
}

void* gfx_gu_get_framebuffer_texture_id(int fb_id) {
    return NULL;
}

void gfx_gu_clear_framebuffer(bool c, bool d) {
    sceGuClearColor(0); // Black background
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
}

void gfx_gu_copy_framebuffer(int fb_dst, int fb_src, int l, int t, bool flip_y, bool use_back) {}
void gfx_gu_resolve_msaa_color_buffer(int fb_id_target, int fb_id_source) {}
bool gfx_gu_start_draw_to_framebuffer(int fb_id, float noise_scale) { return false; }
int gfx_gu_create_framebuffer(void) { return 0; }
void gfx_gu_update_framebuffer_parameters(int fb, uint32_t w, uint32_t h, uint32_t msaa, bool inv_y, bool rt, bool d, bool extract) {}
void gfx_gu_select_texture_fb(int fb_id) {}

void gfx_gu_set_texture_filter(FilteringMode mode) { current_filter_mode = mode; }
FilteringMode gfx_gu_get_texture_filter(void) { return current_filter_mode; }

struct GfxRenderingAPI gfx_gu_api = {
    gfx_gu_get_name,
    gfx_gu_get_max_texture_size,
    gfx_gu_get_clip_parameters,
    gfx_gu_unload_shader,
    gfx_gu_load_shader,
    gfx_gu_create_and_load_new_shader,
    gfx_gu_lookup_shader,
    gfx_gu_shader_get_info,
    gfx_gu_clear_shaders,
    (uint32_t (*)())gfx_gu_new_texture,
    (void (*)(int, uint32_t, bool))gfx_gu_select_texture,
    gfx_gu_upload_texture,
    gfx_gu_set_sampler_parameters,
    gfx_gu_set_depth_mode,
    gfx_gu_set_depth_range,
    gfx_gu_set_viewport,
    gfx_gu_set_scissor,
    gfx_gu_set_use_alpha,
    gfx_gu_draw_triangles,
    gfx_gu_init,
    gfx_gu_on_resize,
    gfx_gu_start_frame,
    gfx_gu_end_frame,
    gfx_gu_finish_render,
    gfx_gu_create_framebuffer,
    gfx_gu_update_framebuffer_parameters,
    gfx_gu_start_draw_to_framebuffer,
    gfx_gu_copy_framebuffer,
    gfx_gu_clear_framebuffer,
    gfx_gu_resolve_msaa_color_buffer,
    gfx_gu_get_framebuffer_texture_id,
    gfx_gu_select_texture_fb,
    gfx_gu_delete_texture,
    gfx_gu_set_texture_filter,
    gfx_gu_get_texture_filter
};
