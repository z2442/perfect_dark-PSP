#ifndef GFX_PSP_GLARE_H
#define GFX_PSP_GLARE_H

#include <stdint.h>

// Exact first-cycle encoding from artifactsConfigureForGlares:
// RGB: (0 - 0) * 0 + ENVIRONMENT
// A:   (TEXEL0 - 0) * ENVIRONMENT + 0
// Fast3D packs RGB in bits 0..15 and alpha in bits 16..27.
// Only the first cycle is active for this draw; do not classify other alpha-only effects.
static inline bool gfxPspIsLightGlare(uint64_t combine_mode, bool one_cycle, bool cloud_surface) {
    return one_cycle && cloud_surface && (combine_mode & 0x0fffffffULL) == 0x0f79bfffULL;
}

enum PspSpriteAlphaMode {
    PSP_SPRITE_ALPHA_NONE = 0,
    PSP_SPRITE_ALPHA_MASK = 1,
    PSP_SPRITE_ALPHA_MODULATE = 2,
};

// text0f153628: RGB = PRIMITIVE, A = TEXEL0_A * PRIMITIVE_A.
// skyRenderFlare: RGB = ENVIRONMENT, A = TEXEL0_A * ENVIRONMENT_A.
// Sun disc: RGB = ENVIRONMENT * TEXEL0, A = ENVIRONMENT_A * TEXEL0_A.
// Restrict these matches to their actual one-cycle translucent surface modes.
static inline uint8_t gfxPspSpriteAlphaMode(uint64_t combine_mode, bool one_cycle,
                                           bool translucent, bool aa_translucent) {
    if (!one_cycle) return PSP_SPRITE_ALPHA_NONE;
    const uint32_t cycle = (uint32_t)(combine_mode & 0x0fffffffULL);
    if (translucent && cycle == 0x0ef97fffU) return PSP_SPRITE_ALPHA_MASK;
    if (aa_translucent && cycle == 0x0f79bfffU) return PSP_SPRITE_ALPHA_MASK;
    if (aa_translucent && cycle == 0x0e7de1f5U) return PSP_SPRITE_ALPHA_MODULATE;
    return PSP_SPRITE_ALPHA_NONE;
}

#endif
