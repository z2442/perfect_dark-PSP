#ifndef GFX_PSP_GE_H
#define GFX_PSP_GE_H

// Private fixed-function state used by gfx_psp.cpp. No GL context or GL library.
// Keep the existing combiner decisions above this layer while owning GE resources.
#include <pspgu.h>
#include <pspge.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <algorithm>
#include <array>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <malloc.h>

namespace {
enum {
    GE_NO_ERROR, GE_OUT_OF_MEMORY, GE_INVALID_VALUE,
    GE_ALPHA_TEST, GE_BLEND, GE_CULL_FACE, GE_DEPTH_TEST, GE_DITHER,
    GE_POLYGON_OFFSET_FILL, GE_SCISSOR_TEST, GE_TEXTURE_2D,
    GE_VERTEX_ARRAY, GE_TEXTURE_COORD_ARRAY, GE_COLOR_ARRAY,
    GE_MODELVIEW, GE_PROJECTION, GE_TEXTURE,
    GE_BACK, GE_FRONT, GE_CW, GE_CCW,
    GE_ALWAYS, GE_EQUAL, GE_LESS, GE_LEQUAL, GE_GEQUAL,
    GE_ZERO, GE_ONE, GE_SRC_ALPHA, GE_ONE_MINUS_SRC_ALPHA, GE_DST_COLOR,
    GE_FLOAT, GE_UNSIGNED_BYTE, GE_UNSIGNED_SHORT_4_4_4_4, GE_UNSIGNED_SHORT_5_6_5,
    GE_RGB, GE_RGBA, GE_ALPHA, GE_LUMINANCE_ALPHA,
    GE_LINEAR, GE_NEAREST, GE_REPEAT, GE_CLAMP_TO_EDGE,
    GE_TEXTURE_MIN_FILTER, GE_TEXTURE_MAG_FILTER, GE_TEXTURE_WRAP_S, GE_TEXTURE_WRAP_T,
    GE_TEXTURE_ENV, GE_TEXTURE_ENV_MODE, GE_MODULATE, GE_REPLACE, GE_COMBINE,
    GE_COMBINE_RGB, GE_COMBINE_ALPHA, GE_PRIMARY_COLOR, GE_SRC_COLOR,
    GE_SRC0_RGB, GE_SRC1_RGB, GE_SRC0_ALPHA, GE_OPERAND0_RGB, GE_OPERAND1_RGB, GE_OPERAND0_ALPHA,
    GE_PACK_ALIGNMENT, GE_UNPACK_ALIGNMENT, GE_MAX_TEXTURE_SIZE, GE_TEXTURE_BINDING_2D,
    GE_TRIANGLES, GE_TRIANGLE_STRIP, GE_SMOOTH,
};
constexpr bool GE_TRUE = true, GE_FALSE = false;
constexpr unsigned GE_COLOR_BUFFER_BIT = 1, GE_DEPTH_BUFFER_BIT = 2;
constexpr int GE_WIDTH = 480, GE_HEIGHT = 272, GE_STRIDE = 512;
constexpr uintptr_t GE_BUFFER_BYTES = GE_STRIDE * GE_HEIGHT * 2;
constexpr size_t GE_LIST_BYTES = 256 * 1024;
alignas(64) uint32_t ge_list[GE_LIST_BYTES / 4];
bool ge_list_open = false;
uintptr_t ge_back = 0, ge_front = GE_BUFFER_BYTES;
std::vector<void *> ge_retired;
int ge_error = GE_NO_ERROR;

static void geBegin() {
    if (!ge_list_open) {
        sceGuStart(GU_DIRECT, ge_list);
        ge_list_open = true;
    }
}
static void geFinish() {
    if (ge_list_open) {
        sceGuFinish();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
        ge_list_open = false;
    }
    for (void *p : ge_retired) free(p);
    ge_retired.clear();
}
static void geReserve(size_t bytes) {
    geBegin();
    // Reserve room for state, list termination and sceGuGetMemory's jump/alignment.
    if (static_cast<size_t>(sceGuCheckList()) + bytes + 4096 >= GE_LIST_BYTES) {
        geFinish();
        geBegin();
    }
}
static void *geBackBuffer() { return reinterpret_cast<void *>(0x44000000u + ge_back); }
static void *geFrontBuffer() { return reinterpret_cast<void *>(0x44000000u + ge_front); }
static void gePresent() {
    geFinish();
    sceDisplayWaitVblankStart();
    // Own both buffers explicitly; composite passes cannot change swap bookkeeping.
    std::swap(ge_front, ge_back);
    sceDisplaySetFrameBuf(reinterpret_cast<void *>(0x04000000u + ge_front),
                         GE_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_565, PSP_DISPLAY_SETBUF_IMMEDIATE);
}
using GeMatrix = std::array<float, 16>;
static GeMatrix geIdentity() {
    GeMatrix m{};
    m[0] = m[5] = m[10] = m[15] = 1;
    return m;
}
struct GeTexture {
    void *pixels = nullptr;
    void *alpha_pixels = nullptr;
    int w = 0, h = 0, stride = 0, psm = GU_PSM_4444;
    int min_filter = GU_NEAREST, mag_filter = GU_NEAREST;
    int wrap_s = GU_REPEAT, wrap_t = GU_REPEAT;
};
std::unordered_map<uint32_t, GeTexture> ge_textures;
uint32_t ge_next_texture = 1, ge_bound_texture = 0;
struct GeArray { const float *ptr = nullptr; int stride = 0; bool enabled = false; };
struct GeState {
    bool texture = false, alpha = false, blend = false, cull = false;
    bool depth = false, depth_write = true, scissor = false, dither = false, offset = false;
    int depth_func = GE_LESS, alpha_func = GE_ALWAYS, alpha_ref = 0;
    int blend_src = GE_ONE, blend_dst = GE_ZERO, face = GE_CCW, cull_face = GE_BACK;
    int vx = 0, vy = 0, vw = GE_WIDTH, vh = GE_HEIGHT;
    int sx = 0, sy = 0, sw = GE_WIDTH, sh = GE_HEIGHT;
    float near_z = 0, far_z = 1;
    int depth_offset = 0;
    uint32_t color = 0xffffffff, clear_color = 0;
    int matrix_mode = GE_MODELVIEW;
    GeMatrix model = geIdentity(), projection = geIdentity();
    std::vector<GeMatrix> model_stack, projection_stack;
    GeArray position, uv, colors;
    int env_mode = GE_MODULATE, env_rgb = GE_MODULATE;
    int env_src_rgb = GE_TEXTURE, env_alpha = GE_MODULATE, env_src_alpha = GE_TEXTURE;
} ge_state;
static void geInit() {
    sceGuInit();
    geBegin();
    sceGuDrawBuffer(GU_PSM_5650, reinterpret_cast<void *>(ge_back), GE_STRIDE);
    sceGuDispBuffer(GE_WIDTH, GE_HEIGHT, reinterpret_cast<void *>(ge_front), GE_STRIDE);
    sceGuDepthBuffer(reinterpret_cast<void *>(2 * GE_BUFFER_BYTES), GE_STRIDE);
    sceGuOffset(2048 - GE_WIDTH / 2, 2048 - GE_HEIGHT / 2);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuDisable(GU_LIGHTING);
    sceGuClearDepth(65535);
    sceGuClearColor(0);
    sceGuScissor(0, 0, GE_WIDTH, GE_HEIGHT);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    geFinish();
    memset(geFrontBuffer(), 0, GE_BUFFER_BYTES);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}
static int geGetError() { int e = ge_error; ge_error = GE_NO_ERROR; return e; }
static void geGetIntegerv(int key, int *value) { *value = key == GE_MAX_TEXTURE_SIZE ? 512 : ge_bound_texture; }
static void gePixelStorei(int, int) {} // All callers supply tightly packed rows.
static void geShadeModel(int) {} // Smooth shading is always applied at submission.
static void geEnable(int cap) {
    switch (cap) {
    case GE_TEXTURE_2D: ge_state.texture = true; break;
    case GE_ALPHA_TEST: ge_state.alpha = true; break;
    case GE_BLEND: ge_state.blend = true; break;
    case GE_CULL_FACE: ge_state.cull = true; break;
    case GE_DEPTH_TEST: ge_state.depth = true; break;
    case GE_SCISSOR_TEST: ge_state.scissor = true; break;
    case GE_DITHER: ge_state.dither = true; break;
    case GE_POLYGON_OFFSET_FILL: ge_state.offset = true; break;
    }
}
static void geDisable(int cap) {
    switch (cap) {
    case GE_TEXTURE_2D: ge_state.texture = false; break;
    case GE_ALPHA_TEST: ge_state.alpha = false; break;
    case GE_BLEND: ge_state.blend = false; break;
    case GE_CULL_FACE: ge_state.cull = false; break;
    case GE_DEPTH_TEST: ge_state.depth = false; break;
    case GE_SCISSOR_TEST: ge_state.scissor = false; break;
    case GE_DITHER: ge_state.dither = false; break;
    case GE_POLYGON_OFFSET_FILL: ge_state.offset = false; break;
    }
}
static GeArray &geArray(int cap) {
    return cap == GE_VERTEX_ARRAY ? ge_state.position : cap == GE_COLOR_ARRAY ? ge_state.colors : ge_state.uv;
}
static void geEnableClientState(int cap) { geArray(cap).enabled = true; }
static void geDisableClientState(int cap) { geArray(cap).enabled = false; }
static void geVertexPointer(int, int, int stride, const void *p) { ge_state.position.ptr = static_cast<const float *>(p); ge_state.position.stride = stride ? stride : 12; }
static void geTexCoordPointer(int, int, int stride, const void *p) { ge_state.uv.ptr = static_cast<const float *>(p); ge_state.uv.stride = stride ? stride : 8; }
static void geColorPointer(int, int, int stride, const void *p) { ge_state.colors.ptr = static_cast<const float *>(p); ge_state.colors.stride = stride ? stride : 16; }
static unsigned geByte(float f) { return static_cast<unsigned>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); }
static uint32_t geColor(float r, float g, float b, float a) { return geByte(r) | geByte(g) << 8 | geByte(b) << 16 | geByte(a) << 24; }
static void geColor4f(float r, float g, float b, float a) { ge_state.color = geColor(r,g,b,a); }
static void geClearColor(float r, float g, float b, float a) { ge_state.clear_color = geColor(r,g,b,a); }
static void geAlphaFunc(int func, float ref) { ge_state.alpha_func = func; ge_state.alpha_ref = geByte(ref); }
static void geBlendFunc(int src, int dst) { ge_state.blend_src = src; ge_state.blend_dst = dst; }
static void geDepthFunc(int func) { ge_state.depth_func = func; }
static void geDepthMask(bool write) { ge_state.depth_write = write; }
static void geDepthRangef(float n, float f) { ge_state.near_z = n; ge_state.far_z = f; }
static void gePolygonOffset(float, float units) { ge_state.depth_offset = static_cast<int>(units); }
static void geFrontFace(int face) { ge_state.face = face; }
static void geCullFace(int face) { ge_state.cull_face = face; }
static void geViewport(int x, int y, int w, int h) { ge_state.vx=x; ge_state.vy=y; ge_state.vw=w; ge_state.vh=h; }
static void geScissor(int x, int y, int w, int h) { ge_state.sx=x; ge_state.sy=y; ge_state.sw=w; ge_state.sh=h; }
static GeMatrix &geMatrix() { return ge_state.matrix_mode == GE_PROJECTION ? ge_state.projection : ge_state.model; }
static std::vector<GeMatrix> &geStack() { return ge_state.matrix_mode == GE_PROJECTION ? ge_state.projection_stack : ge_state.model_stack; }
static void geMatrixMode(int mode) { ge_state.matrix_mode = mode; }
static void geLoadIdentity() { geMatrix() = geIdentity(); }
static void geLoadMatrixf(const float *m) { memcpy(geMatrix().data(), m, 64); }
static void gePushMatrix() { geStack().push_back(geMatrix()); }
static void gePopMatrix() { if (!geStack().empty()) { geMatrix() = geStack().back(); geStack().pop_back(); } }
static void geOrthof(float l, float r, float b, float t, float n, float f) {
    GeMatrix o{};
    o[0]=2/(r-l); o[5]=2/(t-b); o[10]=-2/(f-n); o[15]=1;
    o[12]=-(r+l)/(r-l); o[13]=-(t+b)/(t-b); o[14]=-(f+n)/(f-n);
    GeMatrix a=geMatrix(), result{};
    for (int c=0;c<4;++c) for (int row=0;row<4;++row) for (int k=0;k<4;++k)
        result[c*4+row] += a[k*4+row]*o[c*4+k];
    geMatrix()=result;
}
static void geGenTextures(int n, uint32_t *ids) { for (int i=0;i<n;++i) { ids[i]=ge_next_texture++; ge_textures.emplace(ids[i],GeTexture{}); } }
static void geRetire(void *p) { if (p) { if (ge_list_open) ge_retired.push_back(p); else free(p); } }
static void geDeleteTextures(int n, const uint32_t *ids) {
    for (int i=0;i<n;++i) {
        auto it=ge_textures.find(ids[i]);
        if (it==ge_textures.end()) continue;
        geRetire(it->second.pixels); geRetire(it->second.alpha_pixels);
        ge_textures.erase(it);
        if (ge_bound_texture==ids[i]) ge_bound_texture=0;
    }
}
static void geBindTexture(int, uint32_t id) { ge_bound_texture=id; }
static void geTexParameteri(int, int key, int value) {
    auto it=ge_textures.find(ge_bound_texture); if (it==ge_textures.end()) return;
    GeTexture &t=it->second;
    switch(key) {
    case GE_TEXTURE_MIN_FILTER: t.min_filter=value==GE_LINEAR?GU_LINEAR:GU_NEAREST; break;
    case GE_TEXTURE_MAG_FILTER: t.mag_filter=value==GE_LINEAR?GU_LINEAR:GU_NEAREST; break;
    case GE_TEXTURE_WRAP_S: t.wrap_s=value==GE_CLAMP_TO_EDGE?GU_CLAMP:GU_REPEAT; break;
    case GE_TEXTURE_WRAP_T: t.wrap_t=value==GE_CLAMP_TO_EDGE?GU_CLAMP:GU_REPEAT; break;
    }
}
static int gePot(int v) { int p=1; while(p<v) p*=2; return p; }
static int gePixelBytes(int psm) { return psm==GU_PSM_8888?4:2; }
static void geConvertPixels(void *dst, const void *src, size_t count, int type) {
    if (!src) { memset(dst,0,count*(type==GE_UNSIGNED_BYTE?4:2)); return; }
    if (type==GE_UNSIGNED_BYTE) { memcpy(dst,src,count*4); return; }
    const uint16_t *in=static_cast<const uint16_t *>(src); uint16_t *out=static_cast<uint16_t *>(dst);
    for (size_t i=0;i<count;++i) {
        uint16_t p=in[i];
        out[i]= type==GE_UNSIGNED_SHORT_5_6_5 ? ((p&31)<<11)|(p&0x7e0)|(p>>11)
            : ((p&15)<<12)|((p&0xf0)<<4)|((p&0xf00)>>4)|(p>>12);
    }
}
static void geTexImage2D(int, int, int, int w, int h, int, int, int type, const void *src) {
    auto it=ge_textures.find(ge_bound_texture);
    if (it==ge_textures.end() || w<1 || h<1 || w>512 || h>512) { ge_error=GE_INVALID_VALUE; return; }
    GeTexture &t=it->second;
    int pw=gePot(w),ph=gePot(h),psm=type==GE_UNSIGNED_BYTE?GU_PSM_8888:type==GE_UNSIGNED_SHORT_5_6_5?GU_PSM_5650:GU_PSM_4444;
    int bpp=gePixelBytes(psm), stride=std::max(pw,16/bpp);
    size_t size=static_cast<size_t>(stride)*ph*bpp;
    void *pixels=memalign(64,size);
    if (!pixels) { ge_error=GE_OUT_OF_MEMORY; return; }
    memset(pixels,0,size);
    for(int y=0;y<h;++y) geConvertPixels(static_cast<uint8_t *>(pixels)+y*stride*bpp,
        src?static_cast<const uint8_t *>(src)+y*w*bpp:nullptr,w,type);
    sceKernelDcacheWritebackRange(pixels,size);
    geRetire(t.pixels); geRetire(t.alpha_pixels);
    t.pixels=pixels; t.alpha_pixels=nullptr; t.w=pw; t.h=ph; t.stride=stride; t.psm=psm;
}
static void geTexSubImage2D(int, int, int x, int y, int w, int h, int, int type, const void *src) {
    auto it=ge_textures.find(ge_bound_texture);
    if (it==ge_textures.end()) { ge_error=GE_INVALID_VALUE; return; }
    GeTexture &t=it->second;
    int psm=type==GE_UNSIGNED_BYTE?GU_PSM_8888:type==GE_UNSIGNED_SHORT_5_6_5?GU_PSM_5650:GU_PSM_4444;
    if (!t.pixels || !src || x<0 || y<0 || w<0 || h<0 || x+w>t.w || y+h>t.h || psm!=t.psm) { ge_error=GE_INVALID_VALUE; return; }
    int bpp=gePixelBytes(t.psm); size_t size=static_cast<size_t>(t.stride)*t.h*bpp;
    void *pixels=memalign(64,size);
    if (!pixels) { ge_error=GE_OUT_OF_MEMORY; return; }
    memcpy(pixels,t.pixels,size);
    for(int row=0;row<h;++row) geConvertPixels(static_cast<uint8_t *>(pixels)+((y+row)*t.stride+x)*bpp,
        static_cast<const uint8_t *>(src)+row*w*bpp,w,type);
    sceKernelDcacheWritebackRange(pixels,size);
    geRetire(t.pixels); geRetire(t.alpha_pixels); t.pixels=pixels; t.alpha_pixels=nullptr;
}
static void geTexEnvi(int, int key, int value) {
    switch(key) {
    case GE_TEXTURE_ENV_MODE: ge_state.env_mode=value; break;
    case GE_COMBINE_RGB: ge_state.env_rgb=value; break;
    case GE_SRC0_RGB: ge_state.env_src_rgb=value; break;
    case GE_COMBINE_ALPHA: ge_state.env_alpha=value; break;
    case GE_SRC0_ALPHA: ge_state.env_src_alpha=value; break;
    }
}
static int geCompare(int f) {
    switch(f) { case GE_LESS:return GU_LESS; case GE_LEQUAL:return GU_LEQUAL; case GE_EQUAL:return GU_EQUAL; case GE_GEQUAL:return GU_GEQUAL; default:return GU_ALWAYS; }
}
static int geBlendFactor(int f) {
    switch(f) { case GE_SRC_ALPHA:return GU_SRC_ALPHA; case GE_ONE_MINUS_SRC_ALPHA:return GU_ONE_MINUS_SRC_ALPHA; case GE_DST_COLOR:return GU_DST_COLOR; default:return GU_FIX; }
}
static void geApplyScissor() {
    int x=0,y=0,w=GE_WIDTH,h=GE_HEIGHT;
    if(ge_state.scissor) { x=ge_state.sx; y=GE_HEIGHT-ge_state.sy-ge_state.sh; w=ge_state.sw; h=ge_state.sh; }
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuScissor(std::clamp(x,0,GE_WIDTH),std::clamp(y,0,GE_HEIGHT),std::clamp(x+w,0,GE_WIDTH),std::clamp(y+h,0,GE_HEIGHT));
}
static void geClear(unsigned mask) {
    geReserve(1024);
    sceGuDrawBufferList(GU_PSM_5650,reinterpret_cast<void *>(ge_back),GE_STRIDE);
    geApplyScissor();
    sceGuDepthMask(ge_state.depth_write?GU_FALSE:GU_TRUE);
    sceGuClearColor(ge_state.clear_color); sceGuClearDepth(65535);
    sceGuClear((mask&GE_COLOR_BUFFER_BIT?GU_COLOR_BUFFER_BIT:0)|(mask&GE_DEPTH_BUFFER_BIT?GU_DEPTH_BUFFER_BIT:0));
}
struct GeVertex { float u,v; uint32_t color; float x,y,z; };
static const float *geArrayAt(const GeArray &a,int i) { return reinterpret_cast<const float *>(reinterpret_cast<const uint8_t *>(a.ptr)+i*a.stride); }
static void geDrawArrays(int primitive, int first, int count) {
    if (count<=0 || !ge_state.position.enabled || !ge_state.position.ptr) return;
    // Triangle batches are split only at triangle boundaries. The only strip caller emits four vertices.
    while(count>0) {
        int n=std::min(count,1023);
        if(primitive==GE_TRIANGLES) n-=n%3;
        if(n==0) return;
        geReserve(n*sizeof(GeVertex));
        auto *v=static_cast<GeVertex *>(sceGuGetMemory(n*sizeof(GeVertex)));
        bool combine=ge_state.env_mode==GE_COMBINE;
        bool alpha_only=combine && ge_state.env_rgb==GE_REPLACE && ge_state.env_src_rgb==GE_PRIMARY_COLOR;
        bool texture_alpha=combine && ge_state.env_alpha==GE_REPLACE && ge_state.env_src_alpha==GE_TEXTURE;
        for(int i=0;i<n;++i) {
            const float *p=geArrayAt(ge_state.position,first+i);
            v[i].x=p[0]; v[i].y=p[1]; v[i].z=p[2]; v[i].u=v[i].v=0;
            if(ge_state.uv.enabled && ge_state.uv.ptr) { const float *uv=geArrayAt(ge_state.uv,first+i); v[i].u=uv[0]; v[i].v=uv[1]; }
            v[i].color=ge_state.color;
            if(ge_state.colors.enabled && ge_state.colors.ptr) { const float *c=geArrayAt(ge_state.colors,first+i); v[i].color=geColor(c[0],c[1],c[2],c[3]); }
            if(texture_alpha) v[i].color |= 0xff000000;
        }
        // sceGuGetMemory may return cached memory; publish vertices before PRIM advances the stall.
        sceKernelDcacheWritebackRange(v,n*sizeof(GeVertex));
        sceGuDrawBufferList(GU_PSM_5650,reinterpret_cast<void *>(ge_back),GE_STRIDE);
        sceGuDepthBuffer(reinterpret_cast<void *>(2*GE_BUFFER_BYTES),GE_STRIDE);
        geApplyScissor();
        sceGuViewport(2048-GE_WIDTH/2+ge_state.vx+ge_state.vw/2,
                      2048-GE_HEIGHT/2+GE_HEIGHT-ge_state.vy-ge_state.vh/2,ge_state.vw,ge_state.vh);
        sceGuOffset(2048-GE_WIDTH/2,2048-GE_HEIGHT/2);
        sceGuDepthRange(static_cast<int>(ge_state.near_z*65535),static_cast<int>(ge_state.far_z*65535));
        sceGuDepthOffset(ge_state.offset?ge_state.depth_offset:0);
        sceGuDepthFunc(geCompare(ge_state.depth_func));
        sceGuDepthMask(ge_state.depth_write?GU_FALSE:GU_TRUE);
        sceGuSetStatus(GU_DEPTH_TEST,ge_state.depth); sceGuSetStatus(GU_ALPHA_TEST,ge_state.alpha);
        sceGuAlphaFunc(geCompare(ge_state.alpha_func),ge_state.alpha_ref,255);
        sceGuSetStatus(GU_BLEND,ge_state.blend);
        sceGuBlendFunc(GU_ADD,geBlendFactor(ge_state.blend_src),geBlendFactor(ge_state.blend_dst),
                       ge_state.blend_src==GE_ONE?0xffffff:0,ge_state.blend_dst==GE_ONE?0xffffff:0);
        sceGuSetStatus(GU_CULL_FACE,ge_state.cull);
        sceGuFrontFace((ge_state.face==GE_CW) != (ge_state.cull_face==GE_FRONT)?GU_CW:GU_CCW);
        sceGuSetStatus(GU_DITHER,ge_state.dither); sceGuShadeModel(GU_SMOOTH);
        sceGuDisable(GU_LIGHTING); sceGuEnable(GU_CLIP_PLANES);
        alignas(16) GeMatrix proj=ge_state.projection, model=ge_state.model, identity=geIdentity();
        // sceGuViewport supplies the negative Y scale for our upward clip convention.
        sceGuSetMatrix(GU_PROJECTION,reinterpret_cast<const ScePspFMatrix4 *>(proj.data()));
        sceGuSetMatrix(GU_VIEW,reinterpret_cast<const ScePspFMatrix4 *>(identity.data()));
        sceGuSetMatrix(GU_MODEL,reinterpret_cast<const ScePspFMatrix4 *>(model.data()));
        auto it=ge_textures.find(ge_bound_texture);
        bool textured=ge_state.texture && it!=ge_textures.end() && it->second.pixels;
        sceGuSetStatus(GU_TEXTURE_2D,textured);
        if(textured) {
            GeTexture &t=it->second;
            void *pixels=t.pixels;
            if(alpha_only && !t.alpha_pixels) {
                size_t size=static_cast<size_t>(t.stride)*t.h*gePixelBytes(t.psm);
                t.alpha_pixels=memalign(64,size);
                if(t.alpha_pixels) {
                    if(t.psm==GU_PSM_8888) { auto *dst=static_cast<uint32_t *>(t.alpha_pixels); auto *src=static_cast<const uint32_t *>(t.pixels); for(size_t j=0;j<size/4;++j) dst[j]=src[j]|0xffffff; }
                    else { auto *dst=static_cast<uint16_t *>(t.alpha_pixels); auto *src=static_cast<const uint16_t *>(t.pixels); for(size_t j=0;j<size/2;++j) dst[j]=t.psm==GU_PSM_5650?0xffff:src[j]|0x0fff; }
                    sceKernelDcacheWritebackRange(t.alpha_pixels,size);
                }
            }
            if(alpha_only && t.alpha_pixels) pixels=t.alpha_pixels;
            sceGuTexMode(t.psm,0,0,0); sceGuTexImage(0,t.w,t.h,t.stride,pixels);
            sceGuTexMapMode(GU_TEXTURE_COORDS,0,0); sceGuTexScale(1,1); sceGuTexOffset(0,0);
            sceGuTexFilter(t.min_filter,t.mag_filter); sceGuTexWrap(t.wrap_s,t.wrap_t);
            int tcc=combine && ge_state.env_alpha==GE_REPLACE && ge_state.env_src_alpha==GE_PRIMARY_COLOR?GU_TCC_RGB:GU_TCC_RGBA;
            sceGuTexFunc(ge_state.env_mode==GE_REPLACE?GU_TFX_REPLACE:GU_TFX_MODULATE,tcc);
            sceGuTexFlush();
        }
        sceGuDrawArray(primitive==GE_TRIANGLES?GU_TRIANGLES:GU_TRIANGLE_STRIP,
                       GU_TEXTURE_32BITF|GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_3D,n,nullptr,v);
        first+=n; count-=n;
    }
}
} // namespace
#endif
