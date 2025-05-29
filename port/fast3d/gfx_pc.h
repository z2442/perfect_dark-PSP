#ifndef GFX_PC_H
#define GFX_PC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unordered_map>
#include <list>
#include <cstddef>

#include <PR/gbi.h>

#include "system.h"

#define SCREEN_WIDTH ((int32_t)gfx_current_native_viewport.width)
#define SCREEN_HEIGHT ((int32_t)gfx_current_native_viewport.height)

extern uintptr_t gfxFramebuffer;

struct GfxRenderingAPI;
struct GfxWindowManagerAPI;

struct TextureCacheKey {
    const uint8_t* texture_addr;
    const uint8_t* palette_addrs[2];
    uint8_t fmt, siz;
    uint8_t palette_index;
    uint16_t uls, ult, lrs, lrt;
    uint8_t cms, cmt;
    uint16_t width, height;

    bool operator==(const TextureCacheKey&) const noexcept = default;

    struct Hasher {
        size_t operator()(const TextureCacheKey& key) const noexcept {
            size_t h = 0;
            auto hash_combine = [](size_t& seed, size_t val) {
                seed ^= val + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            };
            hash_combine(h, (uintptr_t)key.texture_addr);
            hash_combine(h, (uintptr_t)key.palette_addrs[0]);
            hash_combine(h, (uintptr_t)key.palette_addrs[1]);
            hash_combine(h, key.fmt);
            hash_combine(h, key.siz);
            hash_combine(h, key.palette_index);
            hash_combine(h, key.uls);
            hash_combine(h, key.ult);
            hash_combine(h, key.lrs);
            hash_combine(h, key.lrt);
            hash_combine(h, key.cms);
            hash_combine(h, key.cmt);
            hash_combine(h, key.width);
            hash_combine(h, key.height);
            return h;
        }
    };
};

typedef std::unordered_map<TextureCacheKey, struct TextureCacheValue, TextureCacheKey::Hasher> TextureCacheMap;
typedef std::pair<const TextureCacheKey, struct TextureCacheValue> TextureCacheNode;

struct TextureCacheValue {
    uint32_t texture_id;
    uint8_t cms, cmt;
    bool linear_filter;
    uint32_t pot_w = 1;
    uint32_t pot_h = 1;

    std::list<struct TextureCacheMapIter>::iterator lru_location;
};

struct TextureCacheMapIter {
    TextureCacheMap::iterator it;
};

extern "C" {

#include "gfx_api.h"

}

#endif
