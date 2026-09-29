#include <cstdint>
#include "gfx_psp_ge.h"
#include "gfx_sdl.h"
#include "gfx_psp_glare.h"

extern "C" { volatile uint8_t g_psp_light_glare = 0; }
extern "C" { volatile uint8_t g_psp_sprite_alpha_mode = PSP_SPRITE_ALPHA_NONE; }

// --- Global state for two-pass N64 combiner emulation ---
extern "C" volatile uint8_t g_force_two_pass; // 0/1: draw second pass using TEXEL1
extern "C" volatile uint8_t g_two_pass_mode; // 0=off, 1=decal(alpha), 2=modulate, 3=additive, 4=additive-alpha, 5=replace
extern "C" volatile float g_tex_s_scale[2];
extern "C" volatile float g_tex_t_scale[2];
extern "C" volatile float g_tex_s_offset[2];
extern "C" volatile float g_tex_t_offset[2];
static uint32_t s_tex_id[2] = {0,0};            // last selected native textures for tile 0/1
static uint32_t s_last_bound_tex = 0;           // tracks most recent texture bound via select_texture
static bool s_last_use_alpha = true;           // last requested blend enable from set_use_alpha
static bool s_last_modulate  = false;          // last requested modulate flag from set_use_alpha
static int s_current_depth_func = GE_LEQUAL; // tracked from set_depth_mode
static bool s_blend_enabled = false;           // applied GE_BLEND enable state
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <ctype.h>



#include "system.h"
#include <pspfpu.h>
#include <pspmath.h>
#include <pspkernel.h>


#include <math.h>

#include <intraFont.h>
#include <pspge.h>
#include <pspgu.h>
#include "psp_home_menu.h"
#include "psp_home_menu_renderer.h"
#include "psp_vfpu.h"

// --- GU-based composite scratch buffer constants ---
// VRAM layout (native 480x272 @ RGB565):
//   color 0: 0x00000 .. 0x43fff; color 1: 0x44000 .. 0x87fff
//   depth:   0x88000 .. 0xcbfff
// Scratch stays at 0x154000 so all composite targets are disjoint from these buffers.
#define GU_COMPOSITE_SCRATCH_OFFSET  0x00154000u
#define GU_COMPOSITE_SCRATCH_END     0x00200000u
#define GU_COMPOSITE_SCRATCH_SIZE    (GU_COMPOSITE_SCRATCH_END - GU_COMPOSITE_SCRATCH_OFFSET)
// CPU-visible uncached address for reads after GU render
#define GU_COMPOSITE_SCRATCH_CPU     ((uint16_t*)(0x44000000u | GU_COMPOSITE_SCRATCH_OFFSET))
// GU draw buffer pointer (VRAM offset, no base)
#define GU_COMPOSITE_SCRATCH_GU      ((void*)GU_COMPOSITE_SCRATCH_OFFSET)
// Max composite size: 256x256 RGBA4444 = 256*256*2 = 128KB -- fits in scratch
#define GU_COMPOSITE_MAX_DIM         256u
#define GU_COMPOSITE_BATCH_ALIGN     64u
#define GU_COMPOSITE_MAX_BATCH_ITEMS 4u
#define GU_COMPOSITE_MIN_QUEUE_DRAIN 4u
#define GU_COMPOSITE_MAX_DEFER_FRAMES 8u
#define GU_COMPOSITE_DYNAMIC_COOLDOWN_FRAMES 2u

// Separate display list for GU composite passes (must not alias the main list)
static uint32_t s_gu_composite_list[4096] __attribute__((aligned(64)));
// Full GE hardware context save/restore (sceGeSaveContext saves all GE regs)
static PspGeContext s_gu_saved_context __attribute__((aligned(64)));

// Sprite vertex layout for GU_SPRITES in GU_TRANSFORM_2D mode:
//   GU_TEXTURE_16BIT | GU_VERTEX_16BIT (no color, no normal)
struct GuSpriteVert {
    int16_t u, v;   // texel coords
    int16_t x, y, z;
} __attribute__((aligned(4), packed));

static inline void psp_clear_ge_errors(void) {
    while (geGetError() != GE_NO_ERROR) {}
}

static inline int psp_check_ge_error(const char* op, int w, int h, int fmt, int type) {
    int err = geGetError();
    if (err != GE_NO_ERROR) {
        const s32 level = (err == GE_OUT_OF_MEMORY) ? LOG_ERROR : LOG_WARNING;
        sysLogPrintf(level, "F3D PSP: %s failed (size=%dx%d fmt=0x%04x type=0x%04x, err=0x%04x)",
                     op, w, h, (unsigned)fmt, (unsigned)type, (unsigned)err);
    }
    return err;
}

#if defined(__GNUC__) || defined(__clang__)
static inline void gfx_prefetch_read(const void* ptr) {
    __builtin_prefetch(ptr, 0, 1);
}
#else
static inline void gfx_prefetch_read(const void*) {}
#endif

static inline uint16_t psp_pack_rgb565_scalar(const uint8_t *rgba) {
    return (uint16_t)(((uint16_t)(rgba[0] >> 3)) |
                      ((uint16_t)(rgba[1] >> 2) << 5) |
                      ((uint16_t)(rgba[2] >> 3) << 11));
}

static inline uint16_t psp_pack_rgba4444_scalar(const uint8_t *rgba) {
    return (uint16_t)(((uint16_t)(rgba[0] >> 4) << 12) |
                      ((uint16_t)(rgba[1] >> 4) << 8)  |
                      ((uint16_t)(rgba[2] >> 4) << 4)  |
                      ((uint16_t)(rgba[3] >> 4)));
}

static inline uint16_t psp_rgb565_to_staging(uint16_t c) {
    // GU/PSP 565 packs R in low bits and B in high bits.
    // The retained upload staging format stores R in high bits and B in low bits.
    return (uint16_t)(((c & 0x001F) << 11) | (c & 0x07E0) | ((c & 0xF800) >> 11));
}

static inline uint16_t psp_rgba4444_to_staging(uint16_t c) {
    // VFPU/GU vt4444 output is nibble-ordered as ABGR in the 16-bit word.
    // GE_UNSIGNED_SHORT_4_4_4_4 expects RGBA.
    return (uint16_t)(((c & 0x000F) << 12) |
                      ((c & 0x00F0) << 4)  |
                      ((c & 0x0F00) >> 4)  |
                      ((c & 0xF000) >> 12));
}

static inline void psp_vfpu_rgba8888_to_rgb565(uint16_t *dst, const uint8_t *src, size_t num_pixels) {
    size_t i = 0;
    const size_t n32 = num_pixels & ~((size_t)31);

    for (; i < n32; i += 32) {
        // Source bytes are RGBA (R,G,B,A); on little-endian this is AABBGGRR per u32,
        // which matches the VFPU color-convert instruction input layout.
        const uint8_t *block_src = src + i * 4u;
        uint8_t *block_dst = reinterpret_cast<uint8_t*>(dst + i);

        __asm__ volatile (
            ".set push               \n"
            ".set noreorder          \n"
            "ulv.q      c000, 0(%[src])    \n"
            "ulv.q      c010, 16(%[src])   \n"
            "ulv.q      c020, 32(%[src])   \n"
            "ulv.q      c030, 48(%[src])   \n"
            "ulv.q      c100, 64(%[src])   \n"
            "ulv.q      c110, 80(%[src])   \n"
            "ulv.q      c120, 96(%[src])   \n"
            "ulv.q      c130, 112(%[src])  \n"

            // 8888 to 565
            "vt5650.q   c200, c000         \n"
            "vt5650.q   c202, c010         \n"
            "vt5650.q   c210, c020         \n"
            "vt5650.q   c212, c030         \n"
            "vt5650.q   c220, c100         \n"
            "vt5650.q   c222, c110         \n"
            "vt5650.q   c230, c120         \n"
            "vt5650.q   c232, c130         \n"

            "usv.q      c200, 0(%[dst])    \n"
            "usv.q      c210, 16(%[dst])   \n"
            "usv.q      c220, 32(%[dst])   \n"
            "usv.q      c230, 48(%[dst])   \n"
            ".set pop                \n"
            :
            : [src] "r" (block_src), [dst] "r" (block_dst)
            : "memory"
        );
    }

    for (; i < num_pixels; ++i) {
        dst[i] = psp_pack_rgb565_scalar(src + i * 4u);
    }
}

static inline void psp_vfpu_rgba8888_to_rgba4444(uint16_t *dst, const uint8_t *src, size_t num_pixels) {
    size_t i = 0;
    const size_t n32 = num_pixels & ~((size_t)31);

    for (; i < n32; i += 32) {
        const uint8_t *block_src = src + i * 4u;
        uint8_t *block_dst = reinterpret_cast<uint8_t*>(dst + i);

        __asm__ volatile (
            ".set push               \n"
            ".set noreorder          \n"
            "ulv.q      c000, 0(%[src])    \n"
            "ulv.q      c010, 16(%[src])   \n"
            "ulv.q      c020, 32(%[src])   \n"
            "ulv.q      c030, 48(%[src])   \n"
            "ulv.q      c100, 64(%[src])   \n"
            "ulv.q      c110, 80(%[src])   \n"
            "ulv.q      c120, 96(%[src])   \n"
            "ulv.q      c130, 112(%[src])  \n"

            // 8888 to 4444
            "vt4444.q   c200, c000         \n"
            "vt4444.q   c202, c010         \n"
            "vt4444.q   c210, c020         \n"
            "vt4444.q   c212, c030         \n"
            "vt4444.q   c220, c100         \n"
            "vt4444.q   c222, c110         \n"
            "vt4444.q   c230, c120         \n"
            "vt4444.q   c232, c130         \n"

            "usv.q      c200, 0(%[dst])    \n"
            "usv.q      c210, 16(%[dst])   \n"
            "usv.q      c220, 32(%[dst])   \n"
            "usv.q      c230, 48(%[dst])   \n"
            ".set pop                \n"
            :
            : [src] "r" (block_src), [dst] "r" (block_dst)
            : "memory"
        );

        for (size_t j = 0; j < 32; ++j) {
            dst[i + j] = psp_rgba4444_to_staging(dst[i + j]);
        }
    }

    for (; i < num_pixels; ++i) {
        dst[i] = psp_pack_rgba4444_scalar(src + i * 4u);
    }
}

static bool s_psp_vfpu_vt4444_checked = false;
static bool s_psp_vfpu_vt4444_ok = true;

static inline bool psp_vfpu_can_vt4444(void) {
    if (s_psp_vfpu_vt4444_checked) {
        return s_psp_vfpu_vt4444_ok;
    }
    s_psp_vfpu_vt4444_checked = true;

    alignas(16) uint8_t sample_src[32 * 4];
    alignas(16) uint16_t sample_vfpu[32];
    alignas(16) uint16_t sample_ref[32];

    for (size_t i = 0; i < 32; ++i) {
        // Distinct nibble patterns across RGBA so channel/nibble order mismatches are caught.
        sample_src[i * 4 + 0] = (uint8_t)((i * 37 + 0x11) & 0xFF);
        sample_src[i * 4 + 1] = (uint8_t)((i * 53 + 0x22) & 0xFF);
        sample_src[i * 4 + 2] = (uint8_t)((i * 79 + 0x33) & 0xFF);
        sample_src[i * 4 + 3] = (uint8_t)((i * 29 + 0x44) & 0xFF);
        sample_ref[i] = psp_pack_rgba4444_scalar(sample_src + i * 4);
    }

    psp_vfpu_rgba8888_to_rgba4444(sample_vfpu, sample_src, 32);
    for (size_t i = 0; i < 32; ++i) {
        if (sample_vfpu[i] != sample_ref[i]) {
            s_psp_vfpu_vt4444_ok = false;
            sysLogPrintf(LOG_WARNING, "F3D PSP: vt4444 validation failed; using scalar RGBA4444 packing");
            break;
        }
    }
    return s_psp_vfpu_vt4444_ok;
}


static int s_matrix_mode = GE_MODELVIEW;

static inline void pdMatrixMode(int mode) {
    if (s_matrix_mode == mode) return;
    geMatrixMode(mode);
    s_matrix_mode = mode;
}
static inline void pdPushMatrix(void) { gePushMatrix(); }
static inline void pdPopMatrix(void) { gePopMatrix(); }
static inline void pdLoadIdentity(void) { geLoadIdentity(); }
static inline void pdLoadMatrixf(const float *m) { geLoadMatrixf(m); }
static inline void pdOrthof(float left, float right, float bottom, float top, float znear, float zfar) {
    geOrthof(left, right, bottom, top, znear, zfar);
}
// ---- Texture metadata & compositor for two-cycle emulation ----
struct CpuTex {
    int w = 0, h = 0;
    uint32_t version = 0;           // increments on each upload to this native tex id
    uint32_t last_frame_updated = 0;
};

static std::unordered_map<uint32_t, CpuTex> s_cpu_tex;   // native tex id -> CPU copy
static uint32_t s_cpu_tex_generation = 1;              // global monotonically increasing version

// Track native texture storage so we can prefer geTexSubImage2D over geTexImage2D.
struct TexAllocInfo {
    uint32_t w = 0;
    uint32_t h = 0;
    int fmt = 0;
    int type = 0;
    bool initialized = false;
};
static std::unordered_map<uint32_t, TexAllocInfo> s_tex_alloc;

struct CompositeKey { uint32_t a, b; uint8_t mode; };
struct CompositeVal { uint32_t texture_id = 0; int w = 0, h = 0; uint32_t ver_a = 0, ver_b = 0; };

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
static std::unordered_set<CompositeKey, CompositeKeyHash, CompositeKeyEq> s_pending_composites;
static uint32_t s_composite_frame_counter = 0;
static uint32_t s_composite_last_drain_frame = 0;
static uint32_t s_last_backbuf_sync_frame = 0xFFFFFFFFu;

struct CompositeBatchItem {
    CompositeKey key{};
    GeTexture src_a{};
    GeTexture src_b{};
    int w = 0;
    int h = 0;
    uint32_t fbw = 0;
    uint32_t scratch_offset = 0;
    uint32_t ver_a = 0;
    uint32_t ver_b = 0;
};

static inline uint16_t pack_rgba4444_pma(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (uint16_t)(((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4));
}



/*
 * Batched GU compositor:
 *  - Cache hits return immediately.
 *  - Cache misses queue a build request and fall back to raw two-pass this draw.
 *  - End-of-frame drains queued requests in GU batches to avoid per-draw sync stalls.
 */
static inline uint32_t gu_align_up_u32(uint32_t val, uint32_t align) {
    return (val + (align - 1u)) & ~(align - 1u);
}

static void gu_emit_composite_item(const CompositeBatchItem &item) {
    auto *bSprite = static_cast<GuSpriteVert *>(sceGuGetMemory(2 * sizeof(GuSpriteVert)));
    bSprite[0] = {0, 0, 0, 0, 0};
    bSprite[1] = {(int16_t)item.w, (int16_t)item.h, (int16_t)item.w, (int16_t)item.h, 0};
    sceKernelDcacheWritebackRange(bSprite, 2 * sizeof(GuSpriteVert));
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuDisable(GU_DITHER);
    sceGuDepthMask(GU_TRUE);
    sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 0);
    sceGuDrawBufferList(GU_PSM_4444, reinterpret_cast<void*>((uintptr_t)item.scratch_offset), (int)item.fbw);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuScissor(0, 0, item.w, item.h);
    sceGuClearColor(0x00000000u);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);

    // Pass 1: base texture
    sceGuDisable(GU_BLEND);
    sceGuTexMode(item.src_a.psm, 0, 0, 1);
    sceGuTexImage(0, item.src_a.w, item.src_a.h, item.src_a.stride, item.src_a.pixels);
    sceGuTexFlush();
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   2, NULL, bSprite);

    // Pass 2: overlay texture with combiner-specific blend mode
    sceGuEnable(GU_BLEND);
    switch (item.key.mode) {
        default:
        case 1:
            sceGuBlendFunc(GU_ADD, GU_FIX, GU_ONE_MINUS_SRC_ALPHA, 0xFFFFFFFFu, 0u);
            break;
        case 2:
            sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_FIX, 0u, 0u);
            break;
        case 3:
        case 4:
            sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFFFFu, 0xFFFFFFFFu);
            break;
        case 5:
            sceGuDisable(GU_BLEND);
            break;
    }
    sceGuTexMode(item.src_b.psm, 0, 0, 1);
    sceGuTexImage(0, item.src_b.w, item.src_b.h, item.src_b.stride, item.src_b.pixels);
    sceGuTexFlush();
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   2, NULL, bSprite);
}

static bool upload_composite_item(const CompositeBatchItem &item) {
    static std::vector<uint16_t> packed_rows;
    const uint16_t *scratch = reinterpret_cast<const uint16_t*>((uintptr_t)(0x44000000u | item.scratch_offset));
    // Read the GE's native ABGR4444 output into the staging format expected by uploads.
    packed_rows.resize((size_t)item.w * item.h);
    for (int y = 0; y < item.h; ++y) {
        for (int x = 0; x < item.w; ++x) {
            packed_rows[(size_t)y * item.w + x] = psp_rgba4444_to_staging(scratch[(size_t)y * item.fbw + x]);
        }
    }
    const void *upload_src = packed_rows.data();

    CompositeVal cv{};
    auto itC = s_composites.find(item.key);
    if (itC != s_composites.end()) cv = itC->second;
    const bool need_alloc = (cv.texture_id == 0) || (cv.w != item.w) || (cv.h != item.h);
    if (cv.texture_id == 0) geGenTextures(1, &cv.texture_id);
    if (cv.texture_id == 0) return false;

    geBindTexture(GE_TEXTURE_2D, cv.texture_id);
    s_last_bound_tex = cv.texture_id;
    if (need_alloc) {
        geTexParameteri(GE_TEXTURE_2D, GE_TEXTURE_MIN_FILTER, GE_LINEAR);
        geTexParameteri(GE_TEXTURE_2D, GE_TEXTURE_MAG_FILTER, GE_LINEAR);
        geTexParameteri(GE_TEXTURE_2D, GE_TEXTURE_WRAP_S, GE_CLAMP_TO_EDGE);
        geTexParameteri(GE_TEXTURE_2D, GE_TEXTURE_WRAP_T, GE_CLAMP_TO_EDGE);
    }
    gePixelStorei(GE_UNPACK_ALIGNMENT, 1);
    psp_clear_ge_errors();

    if (need_alloc) {
        geTexImage2D(GE_TEXTURE_2D, 0, GE_RGBA,
                     (int)item.w, (int)item.h, 0,
                     GE_RGBA, GE_UNSIGNED_SHORT_4_4_4_4, upload_src);
        if (psp_check_ge_error("geTexImage2D GU composite", item.w, item.h,
                               GE_RGBA, GE_UNSIGNED_SHORT_4_4_4_4) != GE_NO_ERROR) {
            if (cv.texture_id != 0) {
                geDeleteTextures(1, &cv.texture_id);
                cv.texture_id = 0;
            }
            return false;
        }
    } else {
        geTexSubImage2D(GE_TEXTURE_2D, 0, 0, 0,
                        (int)item.w, (int)item.h,
                        GE_RGBA, GE_UNSIGNED_SHORT_4_4_4_4, upload_src);
        if (psp_check_ge_error("geTexSubImage2D GU composite", item.w, item.h,
                               GE_RGBA, GE_UNSIGNED_SHORT_4_4_4_4) != GE_NO_ERROR) {
            return false;
        }
    }

    cv.w = item.w;
    cv.h = item.h;
    cv.ver_a = item.ver_a;
    cv.ver_b = item.ver_b;
    s_composites[item.key] = cv;
    return true;
}

static void gfx_psp_process_pending_composites(void) {
    if (s_pending_composites.empty()) return;

    while (!s_pending_composites.empty()) {
        std::vector<CompositeBatchItem> batch;
        batch.reserve((size_t)GU_COMPOSITE_MAX_BATCH_ITEMS);
        uint32_t cursor = GU_COMPOSITE_SCRATCH_OFFSET;

        for (auto it = s_pending_composites.begin(); it != s_pending_composites.end();) {
            const CompositeKey key = *it;
            auto it0 = s_cpu_tex.find(key.a);
            auto it1 = s_cpu_tex.find(key.b);
            if (it0 == s_cpu_tex.end() || it1 == s_cpu_tex.end()) {
                it = s_pending_composites.erase(it);
                continue;
            }

            const CpuTex &A = it0->second;
            const CpuTex &B = it1->second;
            if (A.w == 0 || A.h == 0 || B.w == 0 || B.h == 0 || A.w != B.w || A.h != B.h) {
                it = s_pending_composites.erase(it);
                continue;
            }
            if ((uint32_t)A.w > GU_COMPOSITE_MAX_DIM || (uint32_t)A.h > GU_COMPOSITE_MAX_DIM) {
                it = s_pending_composites.erase(it);
                continue;
            }

            auto itC = s_composites.find(key);
            if (itC != s_composites.end()) {
                const CompositeVal &cv = itC->second;
                if (cv.texture_id != 0 && cv.ver_a == A.version && cv.ver_b == B.version) {
                    it = s_pending_composites.erase(it);
                    continue;
                }
            }

            const uint32_t fbw = ((uint32_t)A.w + 63u) & ~63u;
            const uint32_t scratch_bytes = fbw * (uint32_t)A.h * 2u;
            if (scratch_bytes > GU_COMPOSITE_SCRATCH_SIZE) {
                it = s_pending_composites.erase(it);
                continue;
            }

            const uint32_t slot = gu_align_up_u32(cursor, GU_COMPOSITE_BATCH_ALIGN);
            if ((batch.size() >= (size_t)GU_COMPOSITE_MAX_BATCH_ITEMS) ||
                (slot + scratch_bytes > GU_COMPOSITE_SCRATCH_END)) {
                ++it;
                continue;
            }

            CompositeBatchItem item{};
            item.key = key;
            auto native_a = ge_textures.find(key.a);
            auto native_b = ge_textures.find(key.b);
            if (native_a == ge_textures.end() || native_b == ge_textures.end() ||
                !native_a->second.pixels || !native_b->second.pixels) {
                it = s_pending_composites.erase(it);
                continue;
            }
            item.src_a = native_a->second;
            item.src_b = native_b->second;
            item.w = A.w;
            item.h = A.h;
            item.fbw = fbw;
            item.scratch_offset = slot;
            item.ver_a = A.version;
            item.ver_b = B.version;
            batch.push_back(item);

            cursor = slot + scratch_bytes;
            it = s_pending_composites.erase(it);
        }

        if (batch.empty()) break;

        // Ensure game rendering is on hardware before touching GE directly.
        geFinish();
        sceGeSaveContext(&s_gu_saved_context);
        sceGuStart(GU_DIRECT, s_gu_composite_list);
        for (const CompositeBatchItem &item : batch) {
            gu_emit_composite_item(item);
        }
        sceGuTexSync();
        sceGuFinish();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
        sceGeRestoreContext(&s_gu_saved_context);

        for (const CompositeBatchItem &item : batch) {
            upload_composite_item(item);
        }

        // Process one GU batch per frame to avoid bursty GE list traffic.
        break;
    }
}

static uint32_t get_or_build_composite(uint32_t tex0, uint32_t tex1, uint8_t mode) {
    if (!tex0 || !tex1) return 0;

    auto it0 = s_cpu_tex.find(tex0);
    auto it1 = s_cpu_tex.find(tex1);
    if (it0 == s_cpu_tex.end() || it1 == s_cpu_tex.end()) return 0;

    const CpuTex &A = it0->second, &B = it1->second;
    if (A.w == 0 || A.h == 0 || B.w == 0 || B.h == 0) return 0;
    if (A.w != B.w || A.h != B.h) return 0;

    // Fast-changing textures (e.g. framebuffer feedback paths) are poor
    // candidates for cached GU composites and can cause persistent GU churn.
    const uint32_t frame_now = s_composite_frame_counter;
    if ((frame_now - A.last_frame_updated) <= GU_COMPOSITE_DYNAMIC_COOLDOWN_FRAMES ||
        (frame_now - B.last_frame_updated) <= GU_COMPOSITE_DYNAMIC_COOLDOWN_FRAMES) {
        return 0;
    }

    CompositeKey key{tex0, tex1, mode};
    auto itC = s_composites.find(key);
    if (itC != s_composites.end()) {
        CompositeVal &cv = itC->second;
        if (cv.ver_a == A.version && cv.ver_b == B.version && cv.texture_id != 0)
            return cv.texture_id;
    }

    const int W = A.w, H = A.h;
    if ((uint32_t)W > GU_COMPOSITE_MAX_DIM || (uint32_t)H > GU_COMPOSITE_MAX_DIM) {
        sysLogPrintf(LOG_WARNING, "F3D PSP: composite %dx%d exceeds GU scratch max (%u), skipping",
                     W, H, GU_COMPOSITE_MAX_DIM);
        return 0;
    }

    const uint32_t fbw = ((uint32_t)W + 63u) & ~63u;
    const size_t scratch_bytes = (size_t)fbw * (size_t)H * sizeof(uint16_t);
    if (scratch_bytes > GU_COMPOSITE_SCRATCH_SIZE) {
        sysLogPrintf(LOG_WARNING, "F3D PSP: composite scratch overflow (%zu bytes needed)", scratch_bytes);
        return 0;
    }

    s_pending_composites.insert(key);
    return 0;
}



static bool s_has_texenv_combine = true;

#include <PR/gbi.h>
#include "gfx_rendering_api.h"
#include "gfx_api.h"

static FilteringMode current_filter_mode = FILTER_LINEAR;

#define SCREEN_WIDTH  480
#define SCREEN_HEIGHT 272

static bool es_depth_test  = false;
static bool es_depth_write = false;
static bool es_scissor_test = false;
static int  es_scissor_x = 0, es_scissor_y = 0, es_scissor_w = 0, es_scissor_h = 0;

static float P_matrix[4][4];

static bool s_supports_depth_clamp = false;
static bool s_emulate_depth_clamp = true;
static const float kDepthClampScale = 0.3f;
extern "C" { volatile float g_es1_depth_clamp_scale = 1.0f; }

extern "C" volatile uint8_t g_es1_depth_clamp_active;

static int s_current_draw_fb = 0;
static int s_system_game_fb_primary = -1;

struct PspFramebuffer {
    uint32_t tex = 0;
    uint32_t w = 0, h = 0;
    bool invert_y = false;
    bool allocated = false;
    bool valid = false;
    uint32_t pot_w = 0, pot_h = 0;
};

static std::vector<PspFramebuffer> s_fbs;

static inline void ge_set_texture_2d_enabled(bool enable);
static inline void ge_set_alpha_test_enabled(bool enable);
static inline void ge_set_blend_enabled(bool enable);
static inline void ge_set_depth_test_enabled(bool enable);
static inline void ge_set_blend_func(int src, int dst);
static inline void ge_set_alpha_func(int func, float ref);
static inline void ge_set_depth_mask(bool enable);
static inline void ge_set_depth_func(int func);

static void begin_2d_batch() {
    g_es1_depth_clamp_active = 0;
    pdMatrixMode(GE_PROJECTION);
    pdPushMatrix();
    pdLoadIdentity();
    pdOrthof(0.0f, (float)SCREEN_WIDTH, (float)SCREEN_HEIGHT, 0.0f, -1.0f, 1.0f);
    pdMatrixMode(GE_MODELVIEW);
    pdPushMatrix();
    pdLoadIdentity();
    ge_set_depth_test_enabled(false);
    ge_set_depth_mask(false);
}

static void end_2d_batch() {
    pdMatrixMode(GE_MODELVIEW);
    pdPopMatrix();
    pdMatrixMode(GE_PROJECTION);
    pdPopMatrix();
    ge_set_depth_test_enabled(es_depth_test);
    ge_set_depth_mask(es_depth_write);
    pdMatrixMode(GE_MODELVIEW);
}

extern "C" float g_es1_P[4][4];
extern "C" float g_es1_M[4][4];
extern "C" volatile int g_es1_matrix_dirty;
extern "C" volatile uint8_t g_es1_cull_mode;
extern "C" volatile uint8_t g_es1_alpha_test_enable;
extern "C" volatile float   g_es1_alpha_test_ref;
extern "C" volatile uint8_t g_es1_highp_alpha;
extern "C" volatile uint8_t g_es1_tex0_in_rgb;
extern "C" volatile uint8_t g_es1_force_2d;
extern "C" volatile uint8_t g_es1_base_modulate;
extern "C" volatile uint8_t g_es1_base_color_mode;
extern "C" volatile uint8_t g_es1_prim_rgba[4];
extern "C" volatile uint8_t g_es1_env_rgba[4];
extern "C" volatile uint8_t g_es1_use_tex0;
extern "C" volatile uint8_t g_es1_use_tex1;
extern "C" volatile uint8_t g_es1_text_outline;
extern "C" volatile uint8_t g_es1_front_face_cw;
extern "C" volatile uint8_t g_es1_depth_clamp_active;
extern "C" volatile uint8_t g_es1_pretransformed;

static void geLoadRowMajorMatrixf(const float m[4][4]) {
    pdLoadMatrixf(&m[0][0]);
}

static void load_projection_matrix_with_depth_clamp(const float m[4][4]) {
    if (s_supports_depth_clamp) {
        g_es1_depth_clamp_active = 1;
        g_es1_depth_clamp_scale = 1.0f;
    } else if (!s_emulate_depth_clamp) {
        g_es1_depth_clamp_active = 0;
        g_es1_depth_clamp_scale = 1.0f;
    }
    if (s_emulate_depth_clamp) {
        float adjusted[4][4];
        memcpy(adjusted, m, sizeof(adjusted));
        for (int i = 0; i < 4; ++i) adjusted[i][2] *= kDepthClampScale;
        pdLoadMatrixf(&adjusted[0][0]);
        g_es1_depth_clamp_active = 1;
        g_es1_depth_clamp_scale = kDepthClampScale;
    } else {
        pdLoadMatrixf(&m[0][0]);
        if (!s_supports_depth_clamp) g_es1_depth_clamp_active = 0;
        g_es1_depth_clamp_scale = 1.0f;
    }
}

static bool s_is_2d_mode = false;

static inline float mirror_coord(float t) {
    float i = floorf(t);
    float f = t - i;
    if (((int)i) & 1) return 1.0f - f;
    return f;
}

static void gfx_psp_set_orthographic_projection(float left, float right, float bottom, float top, float near_clip, float far_clip) {
    memset(P_matrix, 0, sizeof(P_matrix));
    P_matrix[0][0] = 2.0f / (right - left);
    P_matrix[1][1] = 2.0f / (top - bottom);
    P_matrix[2][2] = -2.0f / (far_clip - near_clip);
    P_matrix[3][0] = -(right + left) / (right - left);
    P_matrix[3][1] = -(top + bottom) / (top - bottom);
    P_matrix[3][2] = -(far_clip + near_clip) / (far_clip - near_clip);
    P_matrix[3][3] = 1.0f;
}

static void gfx_psp_set_projection_for_2d() {
    pdMatrixMode(GE_PROJECTION);
    pdLoadIdentity();
    pdOrthof(0.0f, (float)SCREEN_WIDTH, (float)SCREEN_HEIGHT, 0.0f, -1.0f, 1.0f);
    pdMatrixMode(GE_MODELVIEW);
    pdLoadIdentity();
}

static void gfx_psp_set_projection_for_3d() {
    pdMatrixMode(GE_PROJECTION);
    load_projection_matrix_with_depth_clamp(g_es1_P);
    pdMatrixMode(GE_MODELVIEW);
    geLoadRowMajorMatrixf(g_es1_M);
}

static uint16_t g_es_zmode = 0;

struct LoadedVertex {
    float x, y, z, w;
    float u, v;
    uint8_t r, g, b, a;
};

struct ShaderProgram {
    uint32_t program_id;
    uint8_t num_inputs;
    bool used_textures[2];
    uint8_t num_floats;
    int attrib_locations[16];
    uint8_t attrib_sizes[16];
    uint8_t num_attribs;
    int frame_count_location;
    int noise_scale_location;
    int three_point_filter_locations[2];
};

static bool current_textures_linear_filter[2] = {false, false};
static bool current_depth_mask = true;
static bool current_poly_offset = false;
static bool s_cull_face_enabled = false;
static int s_cull_face_mode = static_cast<int>(-1);
static int s_front_face_mode = static_cast<int>(-1);
static bool s_vertex_array_enabled = false;
static bool s_texcoord_array_enabled = false;
static bool s_color_array_enabled = false;
static bool s_texture_2d_enabled = false;
static bool s_alpha_test_enabled = false;
static bool s_depth_test_ge_enabled = false;
static bool s_depth_mask_ge_enabled = true;
static int s_depth_func_ge = GE_LESS;
static int s_blend_src_ge = GE_ONE;
static int s_blend_dst_ge = GE_ZERO;
static int s_alpha_func_ge = GE_ALWAYS;
static float s_alpha_ref_ge = 0.0f;
static const void* s_vertex_pointer = nullptr;
static int s_vertex_pointer_stride = -1;
static const void* s_texcoord_pointer = nullptr;
static int s_texcoord_pointer_stride = -1;
static const void* s_color_pointer = nullptr;
static int s_color_pointer_stride = -1;

enum TexEnvModeES1 {
    TEXENV_UNKNOWN = -1,
    TEXENV_MODULATE = 0,
    TEXENV_REPLACE = 1,
    TEXENV_FONT_COMBINE = 2,
    TEXENV_MODULATE_CONST_PRIMARY_ALPHA = 3,
    TEXENV_MODULATE_CONST_TEXTURE_ALPHA = 4,
};
static int s_texenv_mode = TEXENV_UNKNOWN;

static inline void ge_set_texture_2d_enabled(bool enable) {
    if (s_texture_2d_enabled == enable) return;
    if (enable) geEnable(GE_TEXTURE_2D); else geDisable(GE_TEXTURE_2D);
    s_texture_2d_enabled = enable;
}

static inline void ge_set_alpha_test_enabled(bool enable) {
    if (s_alpha_test_enabled == enable) return;
    if (enable) geEnable(GE_ALPHA_TEST); else geDisable(GE_ALPHA_TEST);
    s_alpha_test_enabled = enable;
}

static inline void ge_set_blend_enabled(bool enable) {
    if (s_blend_enabled == enable) return;
    if (enable) geEnable(GE_BLEND); else geDisable(GE_BLEND);
    s_blend_enabled = enable;
}

static inline void ge_set_depth_test_enabled(bool enable) {
    if (s_depth_test_ge_enabled == enable) return;
    if (enable) geEnable(GE_DEPTH_TEST); else geDisable(GE_DEPTH_TEST);
    s_depth_test_ge_enabled = enable;
}

static inline void ge_set_blend_func(int src, int dst) {
    if (s_blend_src_ge == src && s_blend_dst_ge == dst) return;
    geBlendFunc(src, dst);
    s_blend_src_ge = src;
    s_blend_dst_ge = dst;
}

static inline void ge_set_alpha_func(int func, float ref) {
    if (s_alpha_func_ge == func && s_alpha_ref_ge == ref) return;
    geAlphaFunc(func, ref);
    s_alpha_func_ge = func;
    s_alpha_ref_ge = ref;
}

static inline void ge_set_depth_mask(bool enable) {
    if (s_depth_mask_ge_enabled == enable) return;
    geDepthMask(enable ? GE_TRUE : GE_FALSE);
    s_depth_mask_ge_enabled = enable;
}

static inline void ge_set_depth_func(int func) {
    if (s_depth_func_ge == func) return;
    geDepthFunc(func);
    s_depth_func_ge = func;
}

static inline void ge_set_client_state(int array, bool enable, bool& shadow) {
    if (shadow == enable) {
        return;
    }
    if (enable) {
        geEnableClientState(array);
    } else {
        geDisableClientState(array);
    }
    shadow = enable;
}

static inline void ge_set_vertex_array_enabled(bool enable) {
    ge_set_client_state(GE_VERTEX_ARRAY, enable, s_vertex_array_enabled);
}

static inline void ge_set_texcoord_array_enabled(bool enable) {
    ge_set_client_state(GE_TEXTURE_COORD_ARRAY, enable, s_texcoord_array_enabled);
}

static inline void ge_set_color_array_enabled(bool enable) {
    ge_set_client_state(GE_COLOR_ARRAY, enable, s_color_array_enabled);
}

static inline void ge_set_cull_face_enabled(bool enable) {
    if (s_cull_face_enabled == enable) {
        return;
    }
    if (enable) {
        geEnable(GE_CULL_FACE);
    } else {
        geDisable(GE_CULL_FACE);
    }
    s_cull_face_enabled = enable;
}

static inline void ge_set_cull_face_mode(int mode) {
    if (s_cull_face_mode == mode) {
        return;
    }
    geCullFace(mode);
    s_cull_face_mode = mode;
}

static inline void ge_set_front_face_mode(int mode) {
    if (s_front_face_mode == mode) {
        return;
    }
    geFrontFace(mode);
    s_front_face_mode = mode;
}

static inline void ge_set_vertex_pointer(const void* ptr, int stride) {
    if (s_vertex_pointer == ptr && s_vertex_pointer_stride == stride) {
        return;
    }
    geVertexPointer(3, GE_FLOAT, stride, ptr);
    s_vertex_pointer = ptr;
    s_vertex_pointer_stride = stride;
}

static inline void ge_set_texcoord_pointer(const void* ptr, int stride) {
    if (s_texcoord_pointer == ptr && s_texcoord_pointer_stride == stride) {
        return;
    }
    geTexCoordPointer(2, GE_FLOAT, stride, ptr);
    s_texcoord_pointer = ptr;
    s_texcoord_pointer_stride = stride;
}

static inline void ge_set_color_pointer(const void* ptr, int stride) {
    if (s_color_pointer == ptr && s_color_pointer_stride == stride) {
        return;
    }
    geColorPointer(4, GE_FLOAT, stride, ptr);
    s_color_pointer = ptr;
    s_color_pointer_stride = stride;
}

static inline void set_texenv_modulate() {
    if (s_texenv_mode == TEXENV_MODULATE) return;
    geTexEnvi(GE_TEXTURE_ENV, GE_TEXTURE_ENV_MODE, GE_MODULATE);
    s_texenv_mode = TEXENV_MODULATE;
}

static inline void set_texenv_replace() {
    if (s_texenv_mode == TEXENV_REPLACE) return;
    geTexEnvi(GE_TEXTURE_ENV, GE_TEXTURE_ENV_MODE, GE_REPLACE);
    s_texenv_mode = TEXENV_REPLACE;
}

static inline void set_texenv_font_combine() {
    if (s_has_texenv_combine) {
        if (s_texenv_mode == TEXENV_FONT_COMBINE) return;
        geTexEnvi(GE_TEXTURE_ENV, GE_TEXTURE_ENV_MODE, GE_COMBINE);
        geTexEnvi(GE_TEXTURE_ENV, GE_COMBINE_RGB,  GE_REPLACE);
        geTexEnvi(GE_TEXTURE_ENV, GE_SRC0_RGB,     GE_PRIMARY_COLOR);
        geTexEnvi(GE_TEXTURE_ENV, GE_OPERAND0_RGB, GE_SRC_COLOR);
        geTexEnvi(GE_TEXTURE_ENV, GE_COMBINE_ALPHA,  GE_REPLACE);
        geTexEnvi(GE_TEXTURE_ENV, GE_SRC0_ALPHA,     GE_TEXTURE);
        geTexEnvi(GE_TEXTURE_ENV, GE_OPERAND0_ALPHA, GE_SRC_ALPHA);
        s_texenv_mode = TEXENV_FONT_COMBINE;
    } else {
        if (s_texenv_mode != TEXENV_MODULATE) {
            geTexEnvi(GE_TEXTURE_ENV, GE_TEXTURE_ENV_MODE, GE_MODULATE);
        }
        s_texenv_mode = TEXENV_MODULATE;
    }
}

static inline void set_texenv_texture_modulate_with_constant(bool alpha_from_primary) {
    if (s_has_texenv_combine) {
        const int wanted_mode = alpha_from_primary
            ? TEXENV_MODULATE_CONST_PRIMARY_ALPHA
            : TEXENV_MODULATE_CONST_TEXTURE_ALPHA;
        if (s_texenv_mode == wanted_mode) return;
        geTexEnvi(GE_TEXTURE_ENV, GE_TEXTURE_ENV_MODE, GE_COMBINE);
        geTexEnvi(GE_TEXTURE_ENV, GE_COMBINE_RGB, GE_MODULATE);
        geTexEnvi(GE_TEXTURE_ENV, GE_SRC0_RGB, GE_TEXTURE);
        geTexEnvi(GE_TEXTURE_ENV, GE_OPERAND0_RGB, GE_SRC_COLOR);
        geTexEnvi(GE_TEXTURE_ENV, GE_SRC1_RGB, GE_PRIMARY_COLOR);
        geTexEnvi(GE_TEXTURE_ENV, GE_OPERAND1_RGB, GE_SRC_COLOR);
        geTexEnvi(GE_TEXTURE_ENV, GE_COMBINE_ALPHA, GE_REPLACE);
        if (alpha_from_primary) {
            geTexEnvi(GE_TEXTURE_ENV, GE_SRC0_ALPHA, GE_PRIMARY_COLOR);
            geTexEnvi(GE_TEXTURE_ENV, GE_OPERAND0_ALPHA, GE_SRC_ALPHA);
        } else {
            geTexEnvi(GE_TEXTURE_ENV, GE_SRC0_ALPHA, GE_TEXTURE);
            geTexEnvi(GE_TEXTURE_ENV, GE_OPERAND0_ALPHA, GE_SRC_ALPHA);
        }
        s_texenv_mode = wanted_mode;
    } else {
        if (s_texenv_mode != TEXENV_MODULATE) {
            geTexEnvi(GE_TEXTURE_ENV, GE_TEXTURE_ENV_MODE, GE_MODULATE);
        }
        s_texenv_mode = TEXENV_MODULATE;
    }
}


static void gfx_psp_unload_shader(struct ShaderProgram* old_prg) {}
static void gfx_psp_load_shader(struct ShaderProgram* new_prg) {}

static struct ShaderProgram* gfx_psp_create_and_load_new_shader(uint64_t shader_id0, uint32_t shader_id1) {
    static struct ShaderProgram dummy;
    memset(&dummy, 0, sizeof(dummy));
    dummy.used_textures[0] = true;
    return &dummy;
}

static struct ShaderProgram* gfx_psp_lookup_shader(uint64_t shader_id0, uint32_t shader_id1) {
    return gfx_psp_create_and_load_new_shader(shader_id0, shader_id1);
}

static void gfx_psp_shader_get_info(struct ShaderProgram* prg, uint8_t* num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static void gfx_psp_clear_shaders(void) {}

static uint32_t gfx_psp_new_texture(void) {
    uint32_t tex;
    geGenTextures(1, &tex);
    return tex;
}

static void gfx_psp_delete_texture(uint32_t texture_id) {
    geDeleteTextures(1, &texture_id);
    s_cpu_tex.erase(texture_id);
    s_tex_alloc.erase(texture_id);
    for (auto it = s_pending_composites.begin(); it != s_pending_composites.end();) {
        if (it->a == texture_id || it->b == texture_id) it = s_pending_composites.erase(it);
        else ++it;
    }
    for (auto it = s_composites.begin(); it != s_composites.end();) {
        if (it->first.a == texture_id || it->first.b == texture_id) {
            geDeleteTextures(1, &it->second.texture_id);
            it = s_composites.erase(it);
        } else ++it;
    }
    if (s_last_bound_tex == texture_id) s_last_bound_tex = 0;
    for (auto &selected : s_tex_id) if (selected == texture_id) selected = 0;
}

static void gfx_psp_select_texture(int tile, uint32_t texture_id, bool linear_filter) {
    if (tile < 0) tile = 0;
    if (tile > 1) tile = 1;
    s_tex_id[tile] = texture_id;
    if (texture_id != 0 && s_last_bound_tex != texture_id) {
        geBindTexture(GE_TEXTURE_2D, texture_id);
        s_last_bound_tex = texture_id;
    }
    current_textures_linear_filter[tile] = linear_filter;
}

static void gfx_psp_upload_texture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        sysLogPrintf(LOG_WARNING, "F3D PSP: skipped texture upload with zero dimension (%ux%u)", (unsigned)width, (unsigned)height);
        return;
    }
    const size_t num_pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    constexpr size_t kPrefetchDistance = 16;
    static std::vector<uint16_t> rgba4444;
    static std::vector<uint16_t> rgb565;
    static std::vector<uint8_t>  pma8888;
    static std::vector<uint8_t>  rgba8888;
    static bool psp_warned_8888 = false;

    const uint32_t upload_bound_tex = s_last_bound_tex;
    TexAllocInfo *alloc = nullptr;
    if (upload_bound_tex != 0) {
        alloc = &s_tex_alloc[upload_bound_tex];
    }

    bool all_opaque = true;
    const uint8_t* alpha = rgba32_buf + 3;
    for (size_t i = 0; i < num_pixels; ++i) {
        if ((i & 0x1F) == 0) gfx_prefetch_read(alpha + kPrefetchDistance);
        if (*alpha != 255) {
            all_opaque = false;
            break;
        }
        alpha += 4;
    }

    gePixelStorei(GE_UNPACK_ALIGNMENT, 1);
    psp_clear_ge_errors();
    int upload_err = GE_NO_ERROR;
    bool uploaded = false;
    bool rgba4444_ready = false;

    if (all_opaque) {
        rgb565.resize(num_pixels);
        psp_vfpu_rgba8888_to_rgb565(rgb565.data(), rgba32_buf, num_pixels);
        for (size_t i = 0; i < num_pixels; ++i) {
            rgb565[i] = psp_rgb565_to_staging(rgb565[i]);
        }

        rgba4444.resize(num_pixels);
        for (size_t i = 0; i < num_pixels; ++i) {
            uint16_t p = rgb565[i];
            uint16_t r4 = (uint16_t)(((p >> 11) & 0x1F) >> 1);
            uint16_t g4 = (uint16_t)(((p >> 5)  & 0x3F) >> 2);
            uint16_t b4 = (uint16_t)(((p >> 0)  & 0x1F) >> 1);
            rgba4444[i] = (uint16_t)((r4 << 12) | (g4 << 8) | (b4 << 4) | 0x000F);
        }
        rgba4444_ready = true;

        const int fmt565 = GE_RGB;
        const int type565 = GE_UNSIGNED_SHORT_5_6_5;
        const bool can_sub_565 = (alloc != nullptr) && alloc->initialized &&
                                 alloc->w == width && alloc->h == height &&
                                 alloc->fmt == fmt565 && alloc->type == type565;

        if (can_sub_565) {
            geTexSubImage2D(GE_TEXTURE_2D, 0, 0, 0,
                            (int)width, (int)height,
                            fmt565, type565, rgb565.data());
            upload_err = psp_check_ge_error("geTexSubImage2D upload (PSP RGB565 VFPU)", (int)width, (int)height, fmt565, type565);
            uploaded = (upload_err == GE_NO_ERROR);
        }

        if (!uploaded) {
            psp_clear_ge_errors();
            geTexImage2D(GE_TEXTURE_2D, 0, fmt565,
                         (int)width, (int)height, 0,
                         fmt565, type565, rgb565.data());
            upload_err = psp_check_ge_error("geTexImage2D upload (PSP RGB565 VFPU)", (int)width, (int)height, fmt565, type565);
            uploaded = (upload_err == GE_NO_ERROR);
            if (uploaded && alloc != nullptr) {
                alloc->w = width; alloc->h = height;
                alloc->fmt = fmt565; alloc->type = type565;
                alloc->initialized = true;
            }
        }
    }

    if (!uploaded) {
        if (!rgba4444_ready) {
            rgba4444.resize(num_pixels);
            const uint8_t* src = rgba32_buf;

            // Build a premultiplied RGBA8888 staging buffer, then convert to RGBA4444 via VFPU.
            pma8888.resize(num_pixels * 4u);
            uint8_t* pma = pma8888.data();
            for (size_t i = 0; i < num_pixels; ++i) {
                if ((i & 0xF) == 0) gfx_prefetch_read(src + kPrefetchDistance);
                uint8_t r = src[0], g = src[1], b = src[2], a = src[3];
                src += 4;
                pma[0] = (uint8_t)((r * a + 128) >> 8);
                pma[1] = (uint8_t)((g * a + 128) >> 8);
                pma[2] = (uint8_t)((b * a + 128) >> 8);
                pma[3] = a;
                pma += 4;
            }
            if (psp_vfpu_can_vt4444()) {
                psp_vfpu_rgba8888_to_rgba4444(rgba4444.data(), pma8888.data(), num_pixels);
            } else {
                const uint8_t* pma_src = pma8888.data();
                for (size_t i = 0; i < num_pixels; ++i) {
                    rgba4444[i] = psp_pack_rgba4444_scalar(pma_src + i * 4u);
                }
            }
            rgba4444_ready = true;
        }

        const int fmt4444 = GE_RGBA;
        const int type4444 = GE_UNSIGNED_SHORT_4_4_4_4;
        const bool can_sub_4444 = (alloc != nullptr) && alloc->initialized &&
                                  alloc->w == width && alloc->h == height &&
                                  alloc->fmt == fmt4444 && alloc->type == type4444;

        if (can_sub_4444) {
            geTexSubImage2D(GE_TEXTURE_2D, 0, 0, 0,
                            (int)width, (int)height,
                            fmt4444, type4444, rgba4444.data());
            upload_err = psp_check_ge_error("geTexSubImage2D upload (PSP RGBA4444)", (int)width, (int)height, fmt4444, type4444);
            uploaded = (upload_err == GE_NO_ERROR);
        }

        if (!uploaded) {
            psp_clear_ge_errors();
            geTexImage2D(GE_TEXTURE_2D, 0, fmt4444,
                         (int)width, (int)height, 0,
                         fmt4444, type4444, rgba4444.data());
            upload_err = psp_check_ge_error("geTexImage2D upload (PSP RGBA4444)", (int)width, (int)height, fmt4444, type4444);
            uploaded = (upload_err == GE_NO_ERROR);
            if (uploaded && alloc != nullptr) {
                alloc->w = width; alloc->h = height;
                alloc->fmt = fmt4444; alloc->type = type4444;
                alloc->initialized = true;
            }
        }
    }

    if (upload_err != GE_NO_ERROR) {
        rgba8888.resize(num_pixels * 4u);
        const uint16_t* src4444 = rgba4444.data();
        for (size_t i = 0; i < num_pixels; ++i) {
            if ((i & 0x1F) == 0) gfx_prefetch_read(reinterpret_cast<const uint8_t*>(src4444 + i) + kPrefetchDistance);
            uint16_t p = src4444[i];
            uint8_t r4 = (uint8_t)((p >> 12) & 0xF), g4 = (uint8_t)((p >> 8) & 0xF);
            uint8_t b4 = (uint8_t)((p >> 4) & 0xF),  a4 = (uint8_t)(p & 0xF);
            rgba8888[i*4+0] = (uint8_t)((r4<<4)|r4); rgba8888[i*4+1] = (uint8_t)((g4<<4)|g4);
            rgba8888[i*4+2] = (uint8_t)((b4<<4)|b4); rgba8888[i*4+3] = (uint8_t)((a4<<4)|a4);
        }
        gePixelStorei(GE_UNPACK_ALIGNMENT, 1);
        psp_clear_ge_errors();

        const int fmt8888 = GE_RGBA, type8888 = GE_UNSIGNED_BYTE;
        const bool can_sub_8888 = (alloc != nullptr) && alloc->initialized &&
                                  alloc->w == width && alloc->h == height &&
                                  alloc->fmt == fmt8888 && alloc->type == type8888;
        int err8888 = GE_NO_ERROR;
        bool uploaded8888 = false;

        if (can_sub_8888) {
            geTexSubImage2D(GE_TEXTURE_2D, 0, 0, 0,
                            (int)width, (int)height,
                            fmt8888, type8888, rgba8888.data());
            err8888 = psp_check_ge_error("geTexSubImage2D upload fallback (PSP RGBA8888)", (int)width, (int)height, fmt8888, type8888);
            uploaded8888 = (err8888 == GE_NO_ERROR);
        }
        if (!uploaded8888) {
            psp_clear_ge_errors();
            geTexImage2D(GE_TEXTURE_2D, 0, fmt8888,
                         (int)width, (int)height, 0,
                         fmt8888, type8888, rgba8888.data());
            err8888 = psp_check_ge_error("geTexImage2D upload fallback (PSP RGBA8888)", (int)width, (int)height, fmt8888, type8888);
            uploaded8888 = (err8888 == GE_NO_ERROR);
            if (uploaded8888 && alloc != nullptr) {
                alloc->w = width; alloc->h = height;
                alloc->fmt = fmt8888; alloc->type = type8888;
                alloc->initialized = true;
            }
        }
        if (!uploaded8888) return;
        if (!psp_warned_8888) {
            sysLogPrintf(LOG_WARNING, "F3D PSP: falling back to RGBA8888 textures; expect higher memory usage");
            psp_warned_8888 = true;
        }
    }

    // Record versions; the compositor reads the owned native texture storage directly.
    uint32_t bound_tex = 0;
    bound_tex = s_last_bound_tex;
    if (bound_tex != 0) {
        CpuTex &ct = s_cpu_tex[bound_tex];
        ct.w = (int)width; ct.h = (int)height;
        ct.version = ++s_cpu_tex_generation;
        ct.last_frame_updated = s_composite_frame_counter;
    }
}

static uint32_t gfx_cm_to_ge(uint32_t val) {
    switch (val) {
        case G_TX_CLAMP: return GE_CLAMP_TO_EDGE;
        case G_TX_WRAP:  return GE_REPEAT;
    }
    return GE_REPEAT;
}

static void gfx_psp_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    const int filter = (linear_filter && current_filter_mode == FILTER_LINEAR) ? GE_LINEAR : GE_NEAREST;
    geTexParameteri(GE_TEXTURE_2D, GE_TEXTURE_MIN_FILTER, filter);
    geTexParameteri(GE_TEXTURE_2D, GE_TEXTURE_MAG_FILTER, filter);
    const bool mirror_s = (cms & G_TX_MIRROR) != 0;
    const bool mirror_t = (cmt & G_TX_MIRROR) != 0;
    int wrap_s = mirror_s ? GE_REPEAT : ((cms & G_TX_CLAMP) ? GE_CLAMP_TO_EDGE : GE_REPEAT);
    int wrap_t = mirror_t ? GE_REPEAT : ((cmt & G_TX_CLAMP) ? GE_CLAMP_TO_EDGE : GE_REPEAT);
    geTexParameteri(GE_TEXTURE_2D, GE_TEXTURE_WRAP_S, wrap_s);
    geTexParameteri(GE_TEXTURE_2D, GE_TEXTURE_WRAP_T, wrap_t);
}

static void gfx_psp_set_depth_mode(bool depth_test, bool depth_update, bool depth_compare, bool depth_source_prim, uint16_t zmode) {
    es_depth_test  = depth_test;
    es_depth_write = depth_update;
    if (depth_test) {
        ge_set_depth_test_enabled(true);
        ge_set_depth_mask(depth_update);
        current_depth_mask = depth_update;
        if (depth_compare) {
            switch (zmode) {
                case ZMODE_INTER:
                    ge_set_depth_func(GE_LEQUAL); s_current_depth_func = GE_LEQUAL;
                    if (current_poly_offset) { geDisable(GE_POLYGON_OFFSET_FILL); current_poly_offset = false; }
                    break;
                case ZMODE_OPA:
                case ZMODE_XLU:
                    if (depth_source_prim) { ge_set_depth_func(GE_LEQUAL); s_current_depth_func = GE_LEQUAL; }
                    else                   { ge_set_depth_func(GE_LESS);   s_current_depth_func = GE_LESS;   }
                    if (current_poly_offset) { geDisable(GE_POLYGON_OFFSET_FILL); current_poly_offset = false; }
                    break;
                case ZMODE_DEC:
                    ge_set_depth_func(GE_LEQUAL); s_current_depth_func = GE_LEQUAL;
                    geEnable(GE_POLYGON_OFFSET_FILL);
                    gePolygonOffset(-1.0f, -1.0f);
                    current_poly_offset = true;
                    break;
            }
        } else {
            ge_set_depth_func(GE_ALWAYS); s_current_depth_func = GE_ALWAYS;
            if (current_poly_offset) { geDisable(GE_POLYGON_OFFSET_FILL); current_poly_offset = false; }
        }
    } else {
        ge_set_depth_test_enabled(false);
        if (current_poly_offset) { geDisable(GE_POLYGON_OFFSET_FILL); current_poly_offset = false; }
    }
}

static float gfx_adjust_x_for_aspect_ratio(float x) {
    float aspect_ratio = (float)SCREEN_WIDTH / (float)SCREEN_HEIGHT;
    return x * aspect_ratio;
}

static void gfx_psp_set_depth_range(float znear, float zfar) { geDepthRangef(znear, zfar); }
static void gfx_psp_set_viewport(int x, int y, int width, int height) { geViewport(x, y, width, height); }

static void gfx_psp_set_scissor(int x, int y, int width, int height) {
    geEnable(GE_SCISSOR_TEST);
    geScissor(x, y, width, height);
    es_scissor_test = true;
    es_scissor_x = x; es_scissor_y = y; es_scissor_w = width; es_scissor_h = height;
}

static void gfx_psp_set_use_alpha(bool use_alpha, bool modulate) {
    s_last_use_alpha = use_alpha;
    s_last_modulate  = modulate;
    ge_set_blend_enabled(use_alpha);
    if (modulate) ge_set_blend_func(GE_DST_COLOR, GE_ZERO);
    else          ge_set_blend_func(GE_ONE, GE_ONE_MINUS_SRC_ALPHA);
}


static void gfx_psp_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    const int stride_floats = 9;
    const int stride_bytes = stride_floats * sizeof(float);

    bool forced2D = (g_es1_force_2d != 0);
    bool prevDepthTestLocal = es_depth_test;
    bool prevDepthMaskLocal = current_depth_mask;
    if (forced2D) {
        pdMatrixMode(GE_PROJECTION);
        pdPushMatrix();
        pdLoadIdentity();
        pdOrthof(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);
        pdMatrixMode(GE_MODELVIEW);
        pdPushMatrix();
        pdLoadIdentity();
        ge_set_depth_test_enabled(false);
        ge_set_depth_mask(false);
        ge_set_cull_face_enabled(false);
    } else {
        if (g_es1_matrix_dirty) {
            pdMatrixMode(GE_PROJECTION);
            if (g_es1_pretransformed == 1) {
                pdLoadIdentity();
            } else {
                load_projection_matrix_with_depth_clamp(g_es1_P);
            }
            pdMatrixMode(GE_MODELVIEW);
            if (g_es1_pretransformed != 0) {
                pdLoadIdentity();
            } else {
                geLoadRowMajorMatrixf(g_es1_M);
            }
            g_es1_matrix_dirty = 0;
        }
    }

    ge_set_front_face_mode(g_es1_front_face_cw ? GE_CW : GE_CCW);
    if (!forced2D && g_es1_cull_mode == 0) {
        ge_set_cull_face_enabled(false);
    } else if (!forced2D) {
        ge_set_cull_face_enabled(true);
        ge_set_cull_face_mode(g_es1_cull_mode == 1 ? GE_BACK : GE_FRONT);
    }

    pdMatrixMode(GE_MODELVIEW);

    ge_set_vertex_array_enabled(true);
    ge_set_texcoord_array_enabled(true);
    bool colorArrayEnabled = true;
    ge_set_color_array_enabled(true);
    ge_set_color_pointer(buf_vbo + 5, stride_bytes);
    ge_set_vertex_pointer(buf_vbo, stride_bytes);
    ge_set_texcoord_pointer(buf_vbo + 3, stride_bytes);

    ge_set_texture_2d_enabled(g_es1_use_tex0 != 0);

    const bool   prevBlend     = s_last_use_alpha;
    const bool   prevDepthMask = current_depth_mask;
    const int prevDepthFunc = s_current_depth_func;

    // --- Pass 1: base (TEXEL0) ---
    if (g_es1_use_tex0 && s_tex_id[0] != 0) {
        if (s_last_bound_tex != s_tex_id[0]) {
            geBindTexture(GE_TEXTURE_2D, s_tex_id[0]);
            s_last_bound_tex = s_tex_id[0];
        }
    }

    const bool constant_alpha_mask = g_psp_light_glare ||
        g_psp_sprite_alpha_mode == PSP_SPRITE_ALPHA_MASK;
    if (constant_alpha_mask) {
        // Reuse the existing white-RGB/texture-alpha mask. Vertex colors contain
        // the straight environment tint and opacity baked by Fast3D.
        geTexEnvi(GE_TEXTURE_ENV, GE_TEXTURE_ENV_MODE, GE_COMBINE);
        geTexEnvi(GE_TEXTURE_ENV, GE_COMBINE_RGB, GE_REPLACE);
        geTexEnvi(GE_TEXTURE_ENV, GE_SRC0_RGB, GE_PRIMARY_COLOR);
        geTexEnvi(GE_TEXTURE_ENV, GE_COMBINE_ALPHA, GE_MODULATE);
        s_texenv_mode = TEXENV_UNKNOWN;
    } else if (g_psp_sprite_alpha_mode == PSP_SPRITE_ALPHA_MODULATE) {
        // Sun disc: multiply both RGB and alpha, using premultiplied texture/tint.
        set_texenv_modulate();
    } else if (g_es1_use_tex0 && g_es1_highp_alpha && g_es1_alpha_test_enable && !g_es1_tex0_in_rgb) {
        set_texenv_font_combine();
    } else {
        switch (g_es1_base_color_mode) {
            default:
            case 0:
                if (g_es1_use_tex0) set_texenv_replace(); else {
                    if (g_es1_base_color_mode == 1) {
                        // shade only
                    } else {
                        ge_set_color_array_enabled(false); colorArrayEnabled = false;
                        uint8_t r8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[0] : g_es1_env_rgba[0];
                        uint8_t g8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[1] : g_es1_env_rgba[1];
                        uint8_t b8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[2] : g_es1_env_rgba[2];
                        uint8_t a8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[3] : g_es1_env_rgba[3];
                        float af = a8 / 255.0f;
                        geColor4f((r8/255.0f)*af, (g8/255.0f)*af, (b8/255.0f)*af, af);
                    }
                }
                break;
            case 1:
                if (g_es1_use_tex0) set_texenv_modulate();
                break;
            case 2:
            case 3:
                if (g_es1_use_tex0) {
                    if ((g_es1_text_outline == 0) && (g_es1_use_tex1 == 0)) {
                        const bool font_like = g_es1_highp_alpha && g_es1_alpha_test_enable;
                        set_texenv_texture_modulate_with_constant(!font_like);
                    } else
                    {
                    ge_set_color_array_enabled(false); colorArrayEnabled = false;
                    uint8_t r8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[0] : g_es1_env_rgba[0];
                    uint8_t g8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[1] : g_es1_env_rgba[1];
                    uint8_t b8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[2] : g_es1_env_rgba[2];
                    uint8_t a8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[3] : g_es1_env_rgba[3];
                    geColor4f(r8/255.0f, g8/255.0f, b8/255.0f, a8/255.0f);
                    const bool font_like = g_es1_highp_alpha && g_es1_alpha_test_enable;
                    set_texenv_texture_modulate_with_constant(!font_like);
                    }
                } else {
                    if ((g_es1_text_outline == 0) && (g_es1_use_tex1 == 0)) {
                        // Baked per-vertex constant color already drives the fixed-function primary color.
                    } else
                    {
                    ge_set_color_array_enabled(false); colorArrayEnabled = false;
                    uint8_t r8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[0] : g_es1_env_rgba[0];
                    uint8_t g8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[1] : g_es1_env_rgba[1];
                    uint8_t b8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[2] : g_es1_env_rgba[2];
                    uint8_t a8 = (g_es1_base_color_mode == 2) ? g_es1_prim_rgba[3] : g_es1_env_rgba[3];
                    float af = a8 / 255.0f;
                    geColor4f((r8/255.0f)*af, (g8/255.0f)*af, (b8/255.0f)*af, af);
                    }
                }
                break;
        }
    }

    bool want_two_pass = (g_force_two_pass != 0) && g_es1_use_tex1 && (s_tex_id[1] != 0) &&
                         !(g_es1_highp_alpha && g_es1_alpha_test_enable);
    bool using_two_pass = want_two_pass;
    uint32_t composite_tex = 0;
    if (want_two_pass) {
        composite_tex = get_or_build_composite(s_tex_id[0], s_tex_id[1], (uint8_t)g_two_pass_mode);
        if (composite_tex != 0) {
            geBindTexture(GE_TEXTURE_2D, composite_tex);
            s_last_bound_tex = composite_tex;
            s_tex_id[0] = composite_tex;
            using_two_pass = false;
        }
    }

    if (using_two_pass) {
        ge_set_alpha_test_enabled(false);
        ge_set_blend_enabled(false);
    } else {
        bool want_blend = prevBlend;
        if (g_es1_highp_alpha && g_es1_alpha_test_enable) {
            uint8_t const_alpha = 255;
            if (g_es1_base_color_mode == 2)      const_alpha = g_es1_prim_rgba[3];
            else if (g_es1_base_color_mode == 3) const_alpha = g_es1_env_rgba[3];
            if (const_alpha < 255)  want_blend = true;
            else if (!prevBlend)    want_blend = false;
        }
        if (want_blend) {
            ge_set_blend_enabled(true);
            if (s_last_modulate) ge_set_blend_func(GE_DST_COLOR, GE_ZERO);
            else                 ge_set_blend_func(GE_ONE, GE_ONE_MINUS_SRC_ALPHA);
        } else {
            ge_set_blend_enabled(false);
        }

        const bool want_alpha_test = g_es1_alpha_test_enable || (want_blend && !s_last_modulate);
        float alpha_ref = g_es1_alpha_test_enable ? g_es1_alpha_test_ref : 0.0f;
        if (alpha_ref > 0.0f && alpha_ref < 1.0f) alpha_ref = fmaxf(0.0f, alpha_ref - 0.03f);
        ge_set_alpha_test_enabled(want_alpha_test);
        if (want_alpha_test) ge_set_alpha_func(GE_GEQUAL, alpha_ref);
    }

    ge_set_depth_mask(prevDepthMask);
    ge_set_depth_func(prevDepthFunc);

    const bool do_text_outline =
        (g_es1_text_outline != 0) && !using_two_pass &&
        (g_es1_use_tex0 != 0) && (g_es1_use_tex1 != 0) &&
        (s_tex_id[0] != 0) && (s_tex_id[1] != 0);

    if (do_text_outline) {
        if (colorArrayEnabled) { ge_set_color_array_enabled(false); colorArrayEnabled = false; }
        set_texenv_modulate();
        const float af = (float)g_es1_env_rgba[3] / 255.0f;
        if (s_last_bound_tex != s_tex_id[0]) { geBindTexture(GE_TEXTURE_2D, s_tex_id[0]); s_last_bound_tex = s_tex_id[0]; }
        geColor4f((g_es1_prim_rgba[0]/255.0f)*af, (g_es1_prim_rgba[1]/255.0f)*af,
                  (g_es1_prim_rgba[2]/255.0f)*af, af);
        geDrawArrays(GE_TRIANGLES, 0, buf_vbo_num_tris * 3);
        if (s_last_bound_tex != s_tex_id[1]) { geBindTexture(GE_TEXTURE_2D, s_tex_id[1]); s_last_bound_tex = s_tex_id[1]; }
        geColor4f((g_es1_env_rgba[0]/255.0f)*af, (g_es1_env_rgba[1]/255.0f)*af,
                  (g_es1_env_rgba[2]/255.0f)*af, af);
        geDrawArrays(GE_TRIANGLES, 0, buf_vbo_num_tris * 3);
    } else {
        const int saved_src = s_blend_src_ge;
        const int saved_dst = s_blend_dst_ge;
        if (constant_alpha_mask) {
            // White mask * straight tint, alpha = texture alpha * light opacity.
            ge_set_blend_func(GE_SRC_ALPHA, GE_ONE_MINUS_SRC_ALPHA);
        }
        geDrawArrays(GE_TRIANGLES, 0, buf_vbo_num_tris * 3);
        if (constant_alpha_mask) ge_set_blend_func(saved_src, saved_dst);
    }

    // --- Optional Pass 2: raw two-pass overlay (fallback if composite build failed) ---
    if (using_two_pass) {
        if (s_last_bound_tex != s_tex_id[1]) {
            geBindTexture(GE_TEXTURE_2D, s_tex_id[1]);
            s_last_bound_tex = s_tex_id[1];
        }
        set_texenv_replace();
        ge_set_depth_mask(false);
        ge_set_depth_func(GE_EQUAL);
        ge_set_blend_enabled(true);
        float alphaThreshold = 0.01f;
        switch (g_two_pass_mode) {
            default:
            case 1: ge_set_blend_func(GE_ONE, GE_ONE_MINUS_SRC_ALPHA); alphaThreshold = 0.25f; break;
            case 2: ge_set_blend_func(GE_DST_COLOR, GE_ZERO);          alphaThreshold = 0.0f;  break;
            case 3: ge_set_blend_func(GE_ONE, GE_ONE);                  alphaThreshold = 0.0f;  break;
            case 4: ge_set_blend_func(GE_ONE, GE_ONE);                  alphaThreshold = 0.0f;  break;
            case 5: ge_set_blend_enabled(false);                        alphaThreshold = 0.0f;  break;
        }
        if (g_es1_alpha_test_enable && g_es1_alpha_test_ref > alphaThreshold)
            alphaThreshold = g_es1_alpha_test_ref;
        if (alphaThreshold > 0.0f && alphaThreshold < 1.0f)
            alphaThreshold = fmaxf(0.0f, alphaThreshold - 0.03f);
        ge_set_alpha_test_enabled(alphaThreshold > 0.0f);
        if (alphaThreshold > 0.0f) ge_set_alpha_func(GE_GEQUAL, alphaThreshold);

        geDrawArrays(GE_TRIANGLES, 0, buf_vbo_num_tris * 3);

        ge_set_alpha_test_enabled(false);
        ge_set_depth_mask(prevDepthMask);
        ge_set_depth_func(prevDepthFunc);
        if (prevBlend) {
            ge_set_blend_enabled(true);
            if (s_last_modulate) ge_set_blend_func(GE_DST_COLOR, GE_ZERO);
            else                 ge_set_blend_func(GE_ONE, GE_ONE_MINUS_SRC_ALPHA);
        } else {
            ge_set_blend_enabled(false);
        }
        if (s_tex_id[0] != 0 && s_last_bound_tex != s_tex_id[0]) {
            geBindTexture(GE_TEXTURE_2D, s_tex_id[0]);
            s_last_bound_tex = s_tex_id[0];
        }
        if (g_es1_highp_alpha && g_es1_alpha_test_enable && !g_es1_tex0_in_rgb) set_texenv_font_combine();
        else if (g_es1_base_modulate) set_texenv_modulate();
        else set_texenv_replace();
    }

    if (forced2D) {
        pdMatrixMode(GE_MODELVIEW);
        pdPopMatrix();
        pdMatrixMode(GE_PROJECTION);
        pdPopMatrix();
        pdMatrixMode(GE_MODELVIEW);
        ge_set_depth_test_enabled(prevDepthTestLocal);
        ge_set_depth_mask(prevDepthMaskLocal);
    }
}

#define HOME_WIDTH 480
#define HOME_HEIGHT 272
#define HOME_STRIDE 512

struct HomeVertex {
    float x, y, z;
    float r, g, b, a;
};

struct HomeFontVertex {
    float x, y, z;
    float u, v;
    float r, g, b, a;
};

static bool s_home_menu_active;
static bool s_home_menu_capture_requested;
static bool s_home_menu_background_captured;
static uint16_t s_home_menu_background[HOME_STRIDE * HOME_HEIGHT] __attribute__((aligned(64)));
static uint16_t s_home_menu_blur[HOME_STRIDE * HOME_HEIGHT] __attribute__((aligned(64)));
static std::vector<HomeVertex> s_home_vertices;
static std::vector<HomeFontVertex> s_home_font_vertices;
static intraFont *s_home_font;
static uint32_t s_home_font_texture;

static void home_push_vertex(float x, float y, float r, float g, float b, float a) {
    s_home_vertices.push_back({ x, y, 0.0f, r, g, b, a });
}

static void home_push_rect(float x, float y, float width, float height,
                           float r, float g, float b, float a) {
    const float x1 = x + width;
    const float y1 = y + height;
    home_push_vertex(x,  y,  r, g, b, a);
    home_push_vertex(x1, y,  r, g, b, a);
    home_push_vertex(x,  y1, r, g, b, a);
    home_push_vertex(x,  y1, r, g, b, a);
    home_push_vertex(x1, y,  r, g, b, a);
    home_push_vertex(x1, y1, r, g, b, a);
}

static const uint8_t *home_glyph(char character) {
    static const uint8_t letters[26][7] = {
        {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14},
        {30,17,17,17,17,17,30}, {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17}, {14,4,4,4,4,4,14},
        {7,2,2,2,18,18,12}, {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17}, {14,17,17,17,17,17,14},
        {30,17,17,30,16,16,16}, {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4}, {17,17,17,17,17,17,14},
        {17,17,17,17,17,10,4}, {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
    };
    static const uint8_t digits[10][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
        {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
        {14,17,17,15,1,1,14}
    };
    static const uint8_t blank[7] = {0,0,0,0,0,0,0};
    static const uint8_t colon[7] = {0,4,4,0,4,4,0};
    static const uint8_t dash[7] = {0,0,0,31,0,0,0};
    static const uint8_t dot[7] = {0,0,0,0,0,4,4};
    static const uint8_t slash[7] = {1,2,2,4,8,8,16};
    static const uint8_t percent[7] = {17,2,4,8,16,0,17};

    character = (char)toupper((unsigned char)character);
    if (character >= 'A' && character <= 'Z') return letters[character - 'A'];
    if (character >= '0' && character <= '9') return digits[character - '0'];
    if (character == ':') return colon;
    if (character == '-') return dash;
    if (character == '.') return dot;
    if (character == '/') return slash;
    if (character == '%') return percent;
    return blank;
}

static float home_text_width(const char *text, int scale) {
    const size_t length = text ? strlen(text) : 0;
    return length ? (float)(length * 6 * scale - scale) : 0.0f;
}

static uint16_t home_intrafont_get_id(unsigned char character) {
    uint16_t id = 0;

    if (s_home_font == NULL) return 0xffff;
    for (uint16_t i = 0; i < s_home_font->charmap_compr_len; i++) {
        const uint16_t first = s_home_font->charmap_compr[i * 2];
        const uint16_t count = s_home_font->charmap_compr[i * 2 + 1];

        if (character >= first && character < first + count) {
            id = (uint16_t)(id + character - first);
            if (s_home_font->fileType == FILETYPE_PGF) id = s_home_font->charmap[id];
            return id < s_home_font->n_chars ? id : 0xffff;
        }
        id = (uint16_t)(id + count);
    }
    return 0xffff;
}

static float home_intrafont_scale(int fallback_scale) {
    if (fallback_scale >= 3) return 0.86f;
    if (fallback_scale == 2) return 0.72f;
    return 0.54f;
}

static float home_intrafont_width(const char *text, float scale) {
    float width = 0.0f;

    if (text == NULL || s_home_font == NULL) return 0.0f;
    for (; *text != '\0'; text++) {
        const uint16_t id = home_intrafont_get_id((unsigned char)*text);
        if (id != 0xffff) width += s_home_font->glyph[id].advance * scale * 0.25f;
    }
    return width;
}

static void home_push_font_vertex(float x, float y, float u, float v,
                                  float r, float g, float b, float a) {
    s_home_font_vertices.push_back({ x, y, 0.0f, u, v, r, g, b, a });
}

static void home_push_font_quad(const Glyph *glyph, float x, float baseline, float scale,
                                float r, float g, float b, float a) {
    const float texture_size = (float)s_home_font->texWidth;
    const float x0 = x + glyph->left * scale;
    const float y0 = baseline - glyph->top * scale;
    const float x1 = x0 + glyph->width * scale;
    const float y1 = y0 + glyph->height * scale;
    const float u0 = (glyph->x - 0.25f) / texture_size;
    const float v0 = (glyph->y - 0.25f) / texture_size;
    const float u1 = (glyph->x + glyph->width + 0.25f) / texture_size;
    const float v1 = (glyph->y + glyph->height + 0.25f) / texture_size;

    home_push_font_vertex(x0, y0, u0, v0, r, g, b, a);
    home_push_font_vertex(x1, y0, u1, v0, r, g, b, a);
    home_push_font_vertex(x0, y1, u0, v1, r, g, b, a);
    home_push_font_vertex(x0, y1, u0, v1, r, g, b, a);
    home_push_font_vertex(x1, y0, u1, v0, r, g, b, a);
    home_push_font_vertex(x1, y1, u1, v1, r, g, b, a);
}

static bool home_push_intrafont_text(float x, float y, const char *text, int fallback_scale,
                                     bool centered, float r, float g, float b, float a) {
    float scale;
    float baseline;

    if (s_home_font == NULL || s_home_font_texture == 0 || text == NULL) return false;
    scale = home_intrafont_scale(fallback_scale);
    baseline = y + s_home_font->advancey * scale * 0.25f;
    if (centered) x -= home_intrafont_width(text, scale) * 0.5f;

    for (; *text != '\0'; text++) {
        const uint16_t id = home_intrafont_get_id((unsigned char)*text);
        if (id == 0xffff) continue;

        const Glyph *glyph = &s_home_font->glyph[id];
        if (glyph->width != 0 && glyph->height != 0) {
            const uint16_t shadow_id = glyph->shadowID;
            if (shadow_id < s_home_font->n_shadows && s_home_font->shadowscale != 0) {
                const Glyph *shadow = &s_home_font->shadowGlyph[shadow_id];
                const float shadow_scale = scale * 64.0f / s_home_font->shadowscale;
                home_push_font_quad(shadow, x, baseline, shadow_scale, 0.0f, 0.0f, 0.0f, a * 0.70f);
            }
            home_push_font_quad(glyph, x, baseline, scale, r, g, b, a);
        }
        x += glyph->advance * scale * 0.25f;
    }
    return true;
}

static void home_push_text(float x, float y, const char *text, int scale, bool centered,
                           float r, float g, float b, float a) {
    if (text == NULL) return;
    if (home_push_intrafont_text(x, y, text, scale, centered, r, g, b, a)) return;
    if (centered) x -= home_text_width(text, scale) * 0.5f;

    for (; *text; text++, x += 6.0f * scale) {
        const uint8_t *glyph = home_glyph(*text);
        for (int row = 0; row < 7; row++) {
            for (int column = 0; column < 5; column++) {
                if (glyph[row] & (1U << (4 - column))) {
                    home_push_rect(x + column * scale, y + row * scale,
                                   (float)scale, (float)scale, r, g, b, a);
                }
            }
        }
    }
}

static void home_init_intrafont(void) {
    std::vector<uint16_t> rgba4444;
    const uint8_t *source;
    uint32_t width;
    uint32_t cached_height;
    uint32_t byte_width;
    uint32_t row_blocks;

    if (s_home_font != NULL || s_home_font_texture != 0) return;
    if (!intraFontInit()) {
        sysLogPrintf(LOG_WARNING, "PSP HOME: intraFont initialization failed; using fallback font");
        return;
    }

    s_home_font = intraFontLoad("flash0:/font/ltn0.pgf", INTRAFONT_CACHE_ASCII);
    if (s_home_font == NULL || s_home_font->texture == NULL ||
        !(s_home_font->options & INTRAFONT_CACHE_ASCII)) {
        if (s_home_font != NULL) intraFontUnload(s_home_font);
        s_home_font = NULL;
        sysLogPrintf(LOG_WARNING, "PSP HOME: firmware intraFont load failed; using fallback font");
        return;
    }

    width = s_home_font->texWidth;
    cached_height = s_home_font->texHeight;
    byte_width = width >> 1;
    row_blocks = byte_width >> 4;
    source = s_home_font->texture;
    if (width == 0 || width > 512 || cached_height == 0 || cached_height > width || row_blocks == 0) {
        intraFontUnload(s_home_font);
        s_home_font = NULL;
        sysLogPrintf(LOG_WARNING, "PSP HOME: invalid intraFont cache; using fallback font");
        return;
    }

    /* intraFont's PSP cache is swizzled T4. Expand it into a linear
     * square RGBA4444 atlas while retaining intraFont's glyph metrics. */
    rgba4444.assign((size_t)width * width, 0);
    for (uint32_t y = 0; y < cached_height; y++) {
        for (uint32_t xb = 0; xb < byte_width; xb++) {
            const uint32_t block = ((y >> 3) * row_blocks + (xb >> 4)) * 128;
            const uint32_t offset = block + (y & 7) * 16 + (xb & 15);
            const uint8_t packed = source[offset];
            const uint8_t alpha0 = packed & 0x0f;
            const uint8_t alpha1 = packed >> 4;
            const size_t pixel = (size_t)y * width + xb * 2;
            rgba4444[pixel] = (uint16_t)(0xfff0 | alpha0);
            rgba4444[pixel + 1] = (uint16_t)(0xfff0 | alpha1);
        }
    }

    psp_clear_ge_errors();
    geGenTextures(1, &s_home_font_texture);
    geBindTexture(GE_TEXTURE_2D, s_home_font_texture);
    s_last_bound_tex = s_home_font_texture;
    geTexParameteri(GE_TEXTURE_2D, GE_TEXTURE_MIN_FILTER, GE_LINEAR);
    geTexParameteri(GE_TEXTURE_2D, GE_TEXTURE_MAG_FILTER, GE_LINEAR);
    geTexParameteri(GE_TEXTURE_2D, GE_TEXTURE_WRAP_S, GE_CLAMP_TO_EDGE);
    geTexParameteri(GE_TEXTURE_2D, GE_TEXTURE_WRAP_T, GE_CLAMP_TO_EDGE);
    gePixelStorei(GE_UNPACK_ALIGNMENT, 1);
    geTexImage2D(GE_TEXTURE_2D, 0, GE_RGBA, (int)width, (int)width, 0,
                 GE_RGBA, GE_UNSIGNED_SHORT_4_4_4_4, rgba4444.data());

    if (psp_check_ge_error("HOME intraFont atlas upload", (int)width, (int)width,
                           GE_RGBA, GE_UNSIGNED_SHORT_4_4_4_4) != GE_NO_ERROR) {
        geDeleteTextures(1, &s_home_font_texture);
        s_home_font_texture = 0;
        s_last_bound_tex = 0;
        intraFontUnload(s_home_font);
        s_home_font = NULL;
        sysLogPrintf(LOG_WARNING, "PSP HOME: intraFont atlas upload failed; using fallback font");
        return;
    }

    sysLogPrintf(LOG_NOTE, "PSP HOME: loaded firmware intraFont");
}

static void home_blur_background(void) {
    for (int y = 0; y < HOME_HEIGHT; y++) {
        for (int x = 0; x < HOME_WIDTH; x++) {
            const int left = x > 2 ? x - 3 : 0;
            const int right = x + 3 < HOME_WIDTH ? x + 3 : HOME_WIDTH - 1;
            const int up = y > 2 ? y - 3 : 0;
            const int down = y + 3 < HOME_HEIGHT ? y + 3 : HOME_HEIGHT - 1;
            const uint16_t samples[5] = {
                s_home_menu_background[y * HOME_STRIDE + x],
                s_home_menu_background[y * HOME_STRIDE + left],
                s_home_menu_background[y * HOME_STRIDE + right],
                s_home_menu_background[up * HOME_STRIDE + x],
                s_home_menu_background[down * HOME_STRIDE + x],
            };
            uint32_t red = 0, green = 0, blue = 0;
            for (int i = 0; i < 5; i++) {
                red += samples[i] & 0x1f;
                green += (samples[i] >> 5) & 0x3f;
                blue += (samples[i] >> 11) & 0x1f;
            }
            s_home_menu_blur[y * HOME_STRIDE + x] =
                (uint16_t)((red / 5) | ((green / 5) << 5) | ((blue / 5) << 11));
        }
        for (int x = HOME_WIDTH; x < HOME_STRIDE; x++) {
            s_home_menu_blur[y * HOME_STRIDE + x] = 0;
        }
    }
    pdPspMemcpyVfpu(s_home_menu_background, s_home_menu_blur, sizeof(s_home_menu_background));
}

static void home_capture_and_restore_background(void) {
    void *front = geFrontBuffer();
    void *back = geBackBuffer();

    geFinish();
    if (s_home_menu_capture_requested) {
        pdPspMemcpyVfpu(s_home_menu_background, front,
                        sizeof(s_home_menu_background));
        home_blur_background();
        s_home_menu_capture_requested = false;
        s_home_menu_background_captured = true;
    }
    if (s_home_menu_active && s_home_menu_background_captured) {
        pdPspMemcpyVfpu(back, s_home_menu_background,
                        sizeof(s_home_menu_background));
        sceKernelDcacheWritebackRange(back, sizeof(s_home_menu_background));
    }
}

static void home_build_main(int selected, float red, float green, float blue) {
    static const char *items[] = { "Resume Game", "Controller Mapping", "Exit Game" };
    home_push_rect(0, 0, HOME_WIDTH, HOME_HEIGHT, 0, 0, 0, 0.38f);
    home_push_rect(102, 42, 276, 188, 0, 0, 0, 0.66f);
    home_push_text(HOME_WIDTH / 2.0f, 62, "Perfect Dark", 3, true, 1, 1, 0.96f, 1);

    for (int i = 0; i < 3; i++) {
        const float y = 112.0f + i * 40.0f;
        if (selected == i) {
            home_push_rect(124, y - 10, 232, 28, red, green, blue, 0.80f);
        }
        home_push_text(HOME_WIDTH / 2.0f, y, items[i], 2, true,
                       selected == i ? 1.0f : 0.86f,
                       selected == i ? 1.0f : 0.89f,
                       selected == i ? 0.96f : 0.86f, 1.0f);
    }
}

static void home_build_mapping(int selected, const char *status,
                               float red, float green, float blue) {
    char line[96];
    char value[48];
    const int binding_count = pdPspHomeMenuGetBindingCount();
    const int deadzone_row = binding_count;
    const int save_row = binding_count + 1;
    const int reset_row = binding_count + 2;
    const int back_row = binding_count + 3;
    const int total_rows = back_row + 1;
    const int visible_rows = 8;
    int first_row = selected - visible_rows / 2;

    if (first_row < 0) first_row = 0;
    if (first_row + visible_rows > total_rows) first_row = total_rows - visible_rows;
    if (first_row < 0) first_row = 0;

    home_push_rect(0, 0, HOME_WIDTH, HOME_HEIGHT, 0, 0, 0, 0.44f);
    home_push_rect(28, 16, 424, 240, 0, 0, 0, 0.70f);
    home_push_text(HOME_WIDTH / 2.0f, 32, "Controller Mapping", 2, true, 1, 1, 0.96f, 1);

    for (int row = first_row; row < first_row + visible_rows; row++) {
        const float y = 62.0f + (row - first_row) * 21.0f;
        if (row == selected) {
            home_push_rect(48, y - 5, 384, 17, red, green, blue, 0.80f);
        }

        if (row < binding_count) {
            pdPspHomeMenuGetBindingValue(row, value, sizeof(value));
            snprintf(line, sizeof(line), "%s: %s", pdPspHomeMenuGetBindingName(row), value);
        } else if (row == deadzone_row) {
            snprintf(line, sizeof(line), "Deadzone: %d%%", pdPspHomeMenuGetDeadzone());
        } else if (row == save_row) {
            snprintf(line, sizeof(line), "Save pspcontrols.ini");
        } else if (row == reset_row) {
            snprintf(line, sizeof(line), "Reset defaults");
        } else {
            snprintf(line, sizeof(line), "Back");
        }
        home_push_text(62, y, line, 1, false, 1, 1, 0.96f, 1);
    }

    home_push_text(HOME_WIDTH / 2.0f, 238,
                   status && status[0] ? status : "Left/Right change  Cross select  Circle back",
                   1, true, 0.75f, 0.80f, 0.76f, 1);
}

static void home_draw_vertices(void) {
    geViewport(0, 0, HOME_WIDTH, HOME_HEIGHT);
    geDisable(GE_SCISSOR_TEST);
    es_scissor_test = false;
    ge_set_texture_2d_enabled(false);
    ge_set_depth_test_enabled(false);
    es_depth_test = false;
    ge_set_depth_mask(false);
    current_depth_mask = false;
    ge_set_alpha_test_enabled(false);
    ge_set_cull_face_enabled(false);
    ge_set_blend_enabled(true);
    ge_set_blend_func(GE_SRC_ALPHA, GE_ONE_MINUS_SRC_ALPHA);
    s_last_use_alpha = true;
    s_last_modulate = false;
    s_texenv_mode = TEXENV_UNKNOWN;

    pdMatrixMode(GE_PROJECTION);
    pdLoadIdentity();
    pdOrthof(0, HOME_WIDTH, HOME_HEIGHT, 0, -1, 1);
    pdMatrixMode(GE_MODELVIEW);
    pdLoadIdentity();

    if (!s_home_vertices.empty()) {
        const HomeVertex *vertices = s_home_vertices.data();
        ge_set_vertex_array_enabled(true);
        ge_set_texcoord_array_enabled(false);
        ge_set_color_array_enabled(true);
        ge_set_vertex_pointer(&vertices[0].x, sizeof(HomeVertex));
        ge_set_color_pointer(&vertices[0].r, sizeof(HomeVertex));
        geDrawArrays(GE_TRIANGLES, 0, (int)s_home_vertices.size());
    }

    if (!s_home_font_vertices.empty() && s_home_font_texture != 0) {
        const HomeFontVertex *vertices = s_home_font_vertices.data();
        ge_set_texture_2d_enabled(true);
        geBindTexture(GE_TEXTURE_2D, s_home_font_texture);
        s_last_bound_tex = s_home_font_texture;
        set_texenv_modulate();
        ge_set_vertex_array_enabled(true);
        ge_set_texcoord_array_enabled(true);
        ge_set_color_array_enabled(true);
        ge_set_vertex_pointer(&vertices[0].x, sizeof(HomeFontVertex));
        ge_set_texcoord_pointer(&vertices[0].u, sizeof(HomeFontVertex));
        ge_set_color_pointer(&vertices[0].r, sizeof(HomeFontVertex));
        geDrawArrays(GE_TRIANGLES, 0, (int)s_home_font_vertices.size());
        ge_set_texcoord_array_enabled(false);
        ge_set_texture_2d_enabled(false);
        s_texenv_mode = TEXENV_UNKNOWN;
    }

    /* The game backend uses premultiplied blending and must reload its 3D matrices. */
    ge_set_blend_func(GE_ONE, GE_ONE_MINUS_SRC_ALPHA);
    g_es1_matrix_dirty = 1;
    geFinish();
    gePresent();
}

extern "C" void pdPspHomeMenuRendererSetActive(int active) {
    s_home_menu_active = active != 0;
    if (!s_home_menu_active) {
        s_home_menu_capture_requested = false;
        s_home_menu_background_captured = false;
    }
}

extern "C" void pdPspHomeMenuRendererRequestBackground(void) {
    s_home_menu_capture_requested = true;
}

extern "C" void pdPspHomeMenuRendererRender(int selected, int screen, int control_selected,
                                              const char *status, uint8_t red,
                                              uint8_t green, uint8_t blue) {
    const float highlight_red = red / 255.0f;
    const float highlight_green = green / 255.0f;
    const float highlight_blue = blue / 255.0f;

    home_capture_and_restore_background();
    s_home_vertices.clear();
    s_home_font_vertices.clear();
    if (s_home_vertices.capacity() < 32768) s_home_vertices.reserve(32768);
    if (s_home_font_vertices.capacity() < 8192) s_home_font_vertices.reserve(8192);

    if (screen == 1) {
        home_build_mapping(control_selected, status, highlight_red, highlight_green, highlight_blue);
    } else {
        home_build_main(selected, highlight_red, highlight_green, highlight_blue);
    }

    home_draw_vertices();
}

extern "C" void pdPspAssetProgressRender(uint32_t permille, const char *status) {
    char percentage[16];
    const uint32_t clamped = permille > (uint32_t)1000 ? (uint32_t)1000 : permille;
    const float progress = clamped / 1000.0f;

    snprintf(percentage, sizeof(percentage), "%u%%", (unsigned int)(clamped / 10u));
    s_home_vertices.clear();
    s_home_font_vertices.clear();
    if (s_home_vertices.capacity() < 32768) s_home_vertices.reserve(32768);
    if (s_home_font_vertices.capacity() < 8192) s_home_font_vertices.reserve(8192);

    home_push_rect(0, 0, HOME_WIDTH, HOME_HEIGHT, 0.015f, 0.02f, 0.025f, 1.0f);
    home_push_text(HOME_WIDTH / 2.0f, 78, "Perfect Dark", 3, true, 1, 1, 0.96f, 1);
    home_push_text(HOME_WIDTH / 2.0f, 125, "Preparing game data", 2, true,
                   0.86f, 0.90f, 0.94f, 1);
    home_push_rect(70, 164, 340, 18, 0.09f, 0.12f, 0.15f, 1.0f);
    home_push_rect(73, 167, 334.0f * progress, 12, 0.10f, 0.36f, 0.62f, 1.0f);
    home_push_text(HOME_WIDTH / 2.0f, 194, percentage, 2, true, 1, 1, 1, 1);
    home_push_text(HOME_WIDTH / 2.0f, 226,
                   status && status[0] ? status : "Building asset cache",
                   1, true, 0.72f, 0.78f, 0.82f, 1);
    home_draw_vertices();
}

static void gfx_psp_init(void) {
    geInit();

    geViewport(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    geScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    ge_set_texture_2d_enabled(true);
    ge_set_depth_test_enabled(true);
    ge_set_blend_enabled(true);
    ge_set_blend_func(GE_ONE, GE_ONE_MINUS_SRC_ALPHA);
    gePixelStorei(GE_UNPACK_ALIGNMENT, 1);
    gePixelStorei(GE_PACK_ALIGNMENT, 1);
    ge_set_cull_face_enabled(true);
    ge_set_front_face_mode(GE_CCW);
    ge_set_cull_face_mode(GE_BACK);
    geShadeModel(GE_SMOOTH);
    geEnable(GE_DITHER);
    pdMatrixMode(GE_PROJECTION);
    pdLoadIdentity();
    pdMatrixMode(GE_MODELVIEW);
    pdLoadIdentity();
    geClearColor(0.0, 0.0, 0.0, 1.0);

    s_last_use_alpha = true;
    s_last_modulate  = false;
    s_current_depth_func = GE_LEQUAL;
    ge_set_depth_func(GE_LEQUAL);
    s_texenv_mode = TEXENV_UNKNOWN;
    /* Load the firmware font before game assets consume the remaining heap. */
    home_init_intrafont();
}

static void gfx_psp_end_frame(void) {
    ++s_composite_frame_counter;
    if (!s_pending_composites.empty()) {
        const uint32_t frames_since_drain = s_composite_frame_counter - s_composite_last_drain_frame;
        const bool drain_for_queue = s_pending_composites.size() >= GU_COMPOSITE_MIN_QUEUE_DRAIN;
        const bool drain_for_age = frames_since_drain >= GU_COMPOSITE_MAX_DEFER_FRAMES;
        if (drain_for_queue || drain_for_age) {
            gfx_psp_process_pending_composites();
            s_composite_last_drain_frame = s_composite_frame_counter;
        }
    }
    gePresent(gfx_sdl.get_swap_interval());
}

extern "C" { volatile uint8_t g_force_two_pass = 0; }
extern "C" { volatile uint8_t g_two_pass_mode = 0; }
extern "C" { volatile float g_tex_s_scale[2]  = {1.0f, 1.0f}; }
extern "C" { volatile float g_tex_t_scale[2]  = {1.0f, 1.0f}; }
extern "C" { volatile float g_tex_s_offset[2] = {0.0f, 0.0f}; }
extern "C" { volatile float g_tex_t_offset[2] = {0.0f, 0.0f}; }
extern "C" { volatile uint8_t g_es1_alpha_test_enable = 0; }
extern "C" { volatile float   g_es1_alpha_test_ref    = 0.0f; }
extern "C" { volatile uint8_t g_es1_highp_alpha       = 0; }
extern "C" { volatile uint8_t g_es1_tex0_in_rgb       = 1; }
extern "C" { volatile uint8_t g_es1_front_face_cw     = 0; }
extern "C" { volatile uint8_t g_es1_force_2d          = 0; }
extern "C" { volatile uint8_t g_es1_depth_clamp_active = 0; }
extern "C" { volatile uint8_t g_es1_base_modulate     = 1; }
extern "C" { volatile uint8_t g_es1_base_color_mode   = 0; }
extern "C" { volatile uint8_t g_es1_prim_rgba[4]      = {255,255,255,255}; }
extern "C" { volatile uint8_t g_es1_env_rgba[4]       = {255,255,255,255}; }
extern "C" { volatile uint8_t g_es1_use_tex0          = 1; }
extern "C" { volatile uint8_t g_es1_use_tex1          = 0; }
extern "C" { volatile uint8_t g_es1_text_outline      = 0; }
extern "C" { volatile uint8_t g_es1_pretransformed    = 0; }

static void gfx_psp_start_frame(void) { geBegin(); }
static void gfx_psp_finish_render(void) {}
static void gfx_psp_on_resize(void) {}
static const char* gfx_psp_get_name(void) { return "PSP GE (GU)"; }

static int gfx_psp_get_max_texture_size(void) {
    int size = 0;
    geGetIntegerv(GE_MAX_TEXTURE_SIZE, &size);
    if (size <= 0) size = 512;
    if (size < 512) size = 512;
    return (int)size;
}

static struct GfxClipParameters gfx_psp_get_clip_parameters(void) {
    return (struct GfxClipParameters){ false, false };
}

// --- Minimal framebuffer emulation ---

static void ensure_fb_index(int fb_id) {
    if (fb_id < 0) fb_id = 0;
    if ((int)s_fbs.size() <= fb_id) s_fbs.resize(fb_id + 1);
}

static void allocate_fb_texture(PspFramebuffer &fb) {
    if (!fb.allocated) { geGenTextures(1, &fb.tex); fb.allocated = true; }
    geBindTexture(GE_TEXTURE_2D, fb.tex);
    s_last_bound_tex = fb.tex;
    geTexParameteri(GE_TEXTURE_2D, GE_TEXTURE_MIN_FILTER, current_filter_mode == FILTER_LINEAR ? GE_LINEAR : GE_NEAREST);
    geTexParameteri(GE_TEXTURE_2D, GE_TEXTURE_MAG_FILTER, current_filter_mode == FILTER_LINEAR ? GE_LINEAR : GE_NEAREST);
    geTexParameteri(GE_TEXTURE_2D, GE_TEXTURE_WRAP_S, GE_CLAMP_TO_EDGE);
    geTexParameteri(GE_TEXTURE_2D, GE_TEXTURE_WRAP_T, GE_CLAMP_TO_EDGE);
    const uint32_t max_tex = (uint32_t)gfx_psp_get_max_texture_size();
    if (fb.w > max_tex) fb.w = max_tex;
    if (fb.h > max_tex) fb.h = max_tex;
    auto next_pot = [](uint32_t v) -> uint32_t { uint32_t p = 1; while (p < v) p <<= 1; return p; };
    fb.pot_w = next_pot(fb.w ? fb.w : 1);
    fb.pot_h = next_pot(fb.h ? fb.h : 1);
    if (fb.pot_w > max_tex) fb.pot_w = max_tex;
    if (fb.pot_h > max_tex) fb.pot_h = max_tex;
    geTexImage2D(GE_TEXTURE_2D, 0, GE_RGBA, (int)fb.pot_w, (int)fb.pot_h, 0, GE_RGBA, GE_UNSIGNED_SHORT_4_4_4_4, NULL);
}

static void fb_copy_window_into_texture(PspFramebuffer &dst, int src_x0, int src_y0, int src_w, int src_h, bool use_back) {
    if (!dst.allocated || dst.tex == 0 || dst.w == 0 || dst.h == 0) return;
    geBindTexture(GE_TEXTURE_2D, dst.tex);
    s_last_bound_tex = dst.tex;
    const void *pixels = use_back ? geBackBuffer() : geFrontBuffer();
    if (use_back) {
        geFinish();
        s_last_backbuf_sync_frame = s_composite_frame_counter;
    }

    const uint16_t *src565 = static_cast<const uint16_t*>(pixels);
    const int win_w = (int)(gfx_current_dimensions.width ? gfx_current_dimensions.width : (uint32_t)SCREEN_WIDTH);
    const int win_h = (int)(gfx_current_dimensions.height ? gfx_current_dimensions.height : (uint32_t)SCREEN_HEIGHT);
    const uint32_t stride = 512u;

    int sx0=src_x0, sy0=src_y0, sx1=src_x0+src_w, sy1=src_y0+src_h;
    if (sx0<0) sx0=0; if (sy0<0) sy0=0;
    if (sx1>win_w) sx1=win_w; if (sy1>win_h) sy1=win_h;
    const int rw=sx1-sx0, rh=sy1-sy0;
    if (rw<=0||rh<=0) { dst.valid=false; return; }

    const uint32_t region_w=(uint32_t)rw, region_h=(uint32_t)rh;
    const uint32_t copy_w=dst.w, copy_h=dst.h;
    if (copy_w==0||copy_h==0) { dst.valid=false; return; }

    static std::vector<uint16_t> psp_bb_tmp;
    const size_t needed=(size_t)copy_w*(size_t)copy_h;
    if (psp_bb_tmp.size()<needed) psp_bb_tmp.resize(needed);

    const uint32_t factor_x=(copy_w&&(region_w%copy_w)==0)?(region_w/copy_w):0;
    const uint32_t factor_y=(copy_h&&(region_h%copy_h)==0)?(region_h/copy_h):0;

    if (factor_x>=1&&factor_y>=1) {
        const uint32_t samples=factor_x*factor_y;
        for (uint32_t y=0;y<copy_h;++y) {
            uint16_t *drow=psp_bb_tmp.data()+(size_t)y*(size_t)copy_w;
            const uint32_t sy0u=(uint32_t)sy0+y*factor_y;
            for (uint32_t x=0;x<copy_w;++x) {
                const uint32_t sx0u=(uint32_t)sx0+x*factor_x;
                uint32_t sr=0,sg=0,sb=0;
                for (uint32_t sy=0;sy<factor_y;++sy) {
                    const uint16_t *srow=src565+(sy0u+sy)*stride+sx0u;
                    for (uint32_t sxx=0;sxx<factor_x;++sxx) {
                        const uint16_t c=srow[sxx];
                        sr+=c&0x1f; sg+=(c>>5)&0x3f; sb+=(c>>11)&0x1f;
                    }
                }
                const uint16_t r=(uint16_t)(sr/samples),g=(uint16_t)(sg/samples),b=(uint16_t)(sb/samples);
                drow[x]=(uint16_t)(((r>>1)<<12)|((g>>2)<<8)|((b>>1)<<4)|0x000f);
            }
        }
    } else {
        const float step_x=(float)region_w/(float)copy_w, step_y=(float)region_h/(float)copy_h;
        for (uint32_t y=0;y<copy_h;++y) {
            const uint32_t src_y=(uint32_t)sy0+std::min<uint32_t>(region_h-1,(uint32_t)((y+0.5f)*step_y));
            const uint16_t *srow=src565+src_y*stride;
            uint16_t *drow=psp_bb_tmp.data()+(size_t)y*(size_t)copy_w;
            for (uint32_t x=0;x<copy_w;++x) {
                const uint32_t src_x=(uint32_t)sx0+std::min<uint32_t>(region_w-1,(uint32_t)((x+0.5f)*step_x));
                const uint16_t c=srow[src_x];
                const uint16_t r=c&0x1f, g=(c>>5)&0x3f, b=(c>>11)&0x1f;
                drow[x]=(uint16_t)(((r>>1)<<12)|((g>>2)<<8)|((b>>1)<<4)|0x000f);
            }
        }
    }
    gePixelStorei(GE_UNPACK_ALIGNMENT,1);
    geTexSubImage2D(GE_TEXTURE_2D,0,0,0,(int)copy_w,(int)copy_h,GE_RGBA,GE_UNSIGNED_SHORT_4_4_4_4,psp_bb_tmp.data());
    dst.valid=true;
}

static void fb_draw_textured_quad(uint32_t tex, float x, float y, float w, float h, bool invert_v, bool opaque_replace) {
    begin_2d_batch();
    ge_set_texture_2d_enabled(true);
    ge_set_cull_face_enabled(false);
    ge_set_alpha_test_enabled(false);
    ge_set_depth_test_enabled(false);
    ge_set_depth_mask(false);
    if (opaque_replace) ge_set_blend_enabled(false);
    else {
        ge_set_blend_enabled(true);
        ge_set_blend_func(GE_ONE, GE_ONE_MINUS_SRC_ALPHA);
    }

    geBindTexture(GE_TEXTURE_2D, tex);
    s_last_bound_tex = tex;

    const float x0=(float)x, y0=(float)y, x1=(float)(x+w), y1=(float)(y+h);
    const float verts[4*3] = { x0,y0,0, x1,y0,0, x0,y1,0, x1,y1,0 };

    float s_max=1.0f, t_max=1.0f;
    for (const auto &fb : s_fbs) {
        if (fb.allocated && fb.tex == tex) {
            const float pw=(float)(fb.pot_w?fb.pot_w:fb.w), ph=(float)(fb.pot_h?fb.pot_h:fb.h);
            if (pw>0.0f) s_max=(float)((float)fb.w/pw);
            if (ph>0.0f) t_max=(float)((float)fb.h/ph);
            break;
        }
    }
    const float t0=invert_v?t_max:0.0f, t1=invert_v?0.0f:t_max;
    const float uvs[4*2] = { 0,t0, s_max,t0, 0,t1, s_max,t1 };

    ge_set_vertex_array_enabled(true);
    ge_set_texcoord_array_enabled(true);
    ge_set_vertex_pointer(verts, 0);
    ge_set_texcoord_pointer(uvs, 0);
    geDrawArrays(GE_TRIANGLE_STRIP, 0, 4);
    ge_set_texture_2d_enabled(false);
    end_2d_batch();
}

void* gfx_psp_get_framebuffer_texture_id(int fb_id) {
    if (fb_id <= 0) return NULL;
    ensure_fb_index(fb_id);
    const PspFramebuffer &fb = s_fbs[fb_id];
    if (!fb.allocated || fb.tex == 0) return NULL;
    return (void*)(uintptr_t)fb.tex;
}

void gfx_psp_clear_framebuffer(bool c, bool d) {
    unsigned mask = 0;
    if (c) mask |= GE_COLOR_BUFFER_BIT;
    if (d) { ge_set_depth_mask(true); mask |= GE_DEPTH_BUFFER_BIT; }
    const bool restore_scissor = es_scissor_test;
    if (restore_scissor) geDisable(GE_SCISSOR_TEST);
    if (mask) geClear(mask);
    if (restore_scissor) { geEnable(GE_SCISSOR_TEST); geScissor(es_scissor_x,es_scissor_y,es_scissor_w,es_scissor_h); }
    ge_set_depth_mask(current_depth_mask);
}

void gfx_psp_copy_framebuffer(int fb_dst, int fb_src, int l, int t, bool flip_y, bool use_back) {
    ensure_fb_index(fb_src);
    ensure_fb_index(fb_dst);

    if (fb_src == 0 && fb_dst > 0) {
        PspFramebuffer &dst = s_fbs[fb_dst];
        if (!dst.allocated || dst.tex == 0 || dst.w == 0 || dst.h == 0) return;
        (void)flip_y;
        const uint32_t native_w_u = gfx_current_native_viewport.width ? gfx_current_native_viewport.width : 1;
        const uint32_t native_h_u = gfx_current_native_viewport.height ? gfx_current_native_viewport.height : 1;
        const uint32_t win_w_u = gfx_current_dimensions.width ? gfx_current_dimensions.width : (uint32_t)SCREEN_WIDTH;
        const uint32_t win_h_u = gfx_current_dimensions.height ? gfx_current_dimensions.height : (uint32_t)SCREEN_HEIGHT;
        const bool want_full_viewport = (l<0||t<0)||(l==0&&t==0&&dst.w==native_w_u&&dst.h==native_h_u);
        if (want_full_viewport) {
            fb_copy_window_into_texture(dst, 0, 0, (int)win_w_u, (int)win_h_u, use_back);
        } else {
            const float scale_x=(float)win_w_u/(float)native_w_u, scale_y=(float)win_h_u/(float)native_h_u;
            fb_copy_window_into_texture(dst,
                (int)floorf((float)l*scale_x), (int)floorf((float)t*scale_y),
                (int)ceilf((float)dst.w*scale_x), (int)ceilf((float)dst.h*scale_y), use_back);
        }
        return;
    }
    if (fb_src > 0 && fb_dst == 0) {
        const PspFramebuffer &src = s_fbs[fb_src];
        if (!src.allocated || src.tex == 0 || src.w == 0 || src.h == 0) return;
        fb_draw_textured_quad(src.tex, (float)l, (float)t, (float)src.w, (float)src.h, src.invert_y, true);
        return;
    }
    // offscreen->offscreen: not supported without FBOs
}

void gfx_psp_resolve_msaa_color_buffer(int fb_id_target, int fb_id_source) { (void)fb_id_target; (void)fb_id_source; }

bool gfx_psp_start_draw_to_framebuffer(int fb_id, float noise_scale) {
    (void)noise_scale;
    s_current_draw_fb = fb_id;
    return true;
}

int gfx_psp_create_framebuffer(void) {
    int id = (int)s_fbs.size();
    s_fbs.resize(id + 1);
    if (s_system_game_fb_primary < 0) s_system_game_fb_primary = id;
    return id;
}

void gfx_psp_update_framebuffer_parameters(int fb, uint32_t w, uint32_t h, uint32_t msaa, bool inv_y, bool rt, bool d, bool extract) {
    (void)msaa; (void)rt; (void)d; (void)extract;
    if (fb <= 0) return;
    ensure_fb_index(fb);
    PspFramebuffer &dst = s_fbs[fb];
    if (dst.w == w && dst.h == h && dst.allocated) { dst.invert_y = inv_y; return; }
    dst.w = w; dst.h = h; dst.invert_y = inv_y;
    allocate_fb_texture(dst);
    const uint32_t alloc_w = (dst.pot_w ? dst.pot_w : (dst.w ? dst.w : 1));
    const uint32_t alloc_h = (dst.pot_h ? dst.pot_h : (dst.h ? dst.h : 1));
    {
        const size_t px = (size_t)alloc_w * (size_t)alloc_h;
        static std::vector<uint16_t> zeros;
        if (zeros.size() < px) zeros.assign(px, 0x0000);
        gePixelStorei(GE_UNPACK_ALIGNMENT, 1);
        psp_clear_ge_errors();
        geTexImage2D(GE_TEXTURE_2D, 0, GE_RGBA, (int)alloc_w, (int)alloc_h, 0,
                     GE_RGBA, GE_UNSIGNED_SHORT_4_4_4_4, zeros.data());
        if (psp_check_ge_error("geTexImage2D framebuffer alloc", (int)alloc_w, (int)alloc_h,
                               GE_RGBA, GE_UNSIGNED_SHORT_4_4_4_4) != GE_NO_ERROR) {
            dst.valid = false; return;
        }
    }
    dst.valid = false;
}

void gfx_psp_select_texture_fb(int fb_id) {
    if (fb_id <= 0) { geBindTexture(GE_TEXTURE_2D,0); s_tex_id[0]=0; s_last_bound_tex=0; return; }
    ensure_fb_index(fb_id);
    const PspFramebuffer &src = s_fbs[fb_id];
    if (!src.allocated || src.tex == 0) { geBindTexture(GE_TEXTURE_2D,0); s_tex_id[0]=0; s_last_bound_tex=0; return; }
    geBindTexture(GE_TEXTURE_2D, src.tex);
    s_tex_id[0] = src.tex;
    s_last_bound_tex = src.tex;
}

void gfx_psp_set_texture_filter(FilteringMode mode) { current_filter_mode = mode; }
FilteringMode gfx_psp_get_texture_filter(void) { return current_filter_mode; }

struct GfxRenderingAPI gfx_psp_api = {
    gfx_psp_get_name,
    gfx_psp_get_max_texture_size,
    gfx_psp_get_clip_parameters,
    gfx_psp_unload_shader,
    gfx_psp_load_shader,
    gfx_psp_create_and_load_new_shader,
    gfx_psp_lookup_shader,
    gfx_psp_shader_get_info,
    gfx_psp_clear_shaders,
    gfx_psp_new_texture,
    gfx_psp_select_texture,
    gfx_psp_upload_texture,
    gfx_psp_set_sampler_parameters,
    gfx_psp_set_depth_mode,
    gfx_psp_set_depth_range,
    gfx_psp_set_viewport,
    gfx_psp_set_scissor,
    gfx_psp_set_use_alpha,
    gfx_psp_draw_triangles,
    gfx_psp_init,
    gfx_psp_on_resize,
    gfx_psp_start_frame,
    gfx_psp_end_frame,
    gfx_psp_finish_render,
    gfx_psp_create_framebuffer,
    gfx_psp_update_framebuffer_parameters,
    gfx_psp_start_draw_to_framebuffer,
    gfx_psp_copy_framebuffer,
    gfx_psp_clear_framebuffer,
    gfx_psp_resolve_msaa_color_buffer,
    gfx_psp_get_framebuffer_texture_id,
    gfx_psp_select_texture_fb,
    gfx_psp_delete_texture,
    gfx_psp_set_texture_filter,
    gfx_psp_get_texture_filter
};
