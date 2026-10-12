// Client-side command replay. See net_render.h.
#define NET_RENDER_CLIENT

#include "net_render.h"
#include "net_render_common.h"
#include "net_render_ops.h"

#include "data_win.h"
#include "log.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>

// ===[ Limits for values received from the server (see the validation notes in tools/gen_net_render.py) ]===
#define RD_MAX_SURFACE 8192            // surfaces / sprites created from surfaces may not be larger than this in either dimension
#define RD_MAX_IDS (1u << 16)          // largest server-side surface / sprite / handle id we keep a mapping for
#define RD_MAX_PRIM_VERTICES (1 << 20) // vertices per primitive
#define RD_MAX_WINDOW 65536

struct NetRenderDecoder {
    Renderer* renderer;
    Runner* runner;
    DataWin* dw;
    int32_t winW, winH;

    int32_t* surfMap; uint32_t surfMapCount;       // server surface id -> local id (-1 unknown)
    int32_t* tpagMap; uint32_t tpagMapCount;       // (server tpag - origTpag) -> local tpag
    int32_t* spriteMap; uint32_t spriteMapCount;   // (server sprite - origSprite) -> local sprite
    int32_t* uniMap; uint32_t uniMapCount;         // (virtual handle - base) -> local uniform / sampler
    uint32_t origTpag, origSprite;

    VertexBuffer vb;  // reused so the renderer keeps one GPU buffer object for dynamic draws
    VertexFormat vf;
    int32_t primVerts; // vertices submitted to the current primitive
    void (*frameBeginHook)(void*);
    void* frameBeginData;
};

typedef void (*RrDecodeFn)(NetRenderDecoder* d, NetReader* r);

// ===[ ID translation ]===

static bool growInts(int32_t** arr, uint32_t* count, uint32_t needed) {
    if (needed > RD_MAX_IDS) return false; // hostile ids must not make us allocate gigabytes
    if (needed <= *count) return true;
    uint32_t newCount = *count ? *count : 16;
    while (newCount < needed) newCount *= 2;
    *arr = (int32_t*) safeRealloc(*arr, newCount * sizeof(int32_t));
    for (uint32_t i = *count; i < newCount; i++) (*arr)[i] = -1;
    *count = newCount;
    return true;
}

static int32_t rdMapSurface(NetRenderDecoder* d, int32_t id) {
    if (id < 0) return id;
    if ((uint32_t) id < d->surfMapCount) return d->surfMap[id];
    return -1;
}

static int32_t rdMapTpag(NetRenderDecoder* d, int32_t id) {
    if (id < 0 || (uint32_t) id < d->origTpag) return id;
    uint32_t slot = (uint32_t) id - d->origTpag;
    if (slot < d->tpagMapCount) return d->tpagMap[slot];
    return -1;
}

static int32_t rdMapSprite(NetRenderDecoder* d, int32_t id) {
    if (id < 0 || (uint32_t) id < d->origSprite) return id;
    uint32_t slot = (uint32_t) id - d->origSprite;
    if (slot < d->spriteMapCount) return d->spriteMap[slot];
    return -1;
}

static uint32_t rdMapTexture(NetRenderDecoder* d, uint32_t h) {
    if (h == 0 || h == 0xFFFFFFFFu) return h;
    if (h & NET_SURFACE_TEXTURE_FLAG) {
        int32_t local = rdMapSurface(d, (int32_t) (h & ~NET_SURFACE_TEXTURE_FLAG));
        return local < 0 ? 0 : (NET_SURFACE_TEXTURE_FLAG | (uint32_t) local);
    }
    int32_t t = rdMapTpag(d, (int32_t) h - 1);
    return t < 0 ? 0 : (uint32_t) t + 1;
}

static int32_t rdMapVirtual(NetRenderDecoder* d, int32_t v) {
    if (v < NET_VIRTUAL_HANDLE_BASE) return v;
    uint32_t i = (uint32_t) (v - NET_VIRTUAL_HANDLE_BASE);
    return i < d->uniMapCount ? d->uniMap[i] : -1;
}
static int32_t rdMapSampler(NetRenderDecoder* d, int32_t v) { return rdMapVirtual(d, v); }
static int32_t rdMapUniform(NetRenderDecoder* d, int32_t v) { return rdMapVirtual(d, v); }

static void mapSurface(NetRenderDecoder* d, int32_t serverId, int32_t localId) {
    if (serverId < 0) return;
    if (!growInts(&d->surfMap, &d->surfMapCount, (uint32_t) serverId + 1)) return;
    d->surfMap[serverId] = localId;
}

// The GL renderers dereference the camera of the current view and the application surface without checking them
static bool rdViewOk(NetRenderDecoder* d) {
    Runner* r = d->runner;
    int32_t vc = r->viewsEnabled ? r->viewCurrent : 0;
    if (vc < 0 || vc >= MAX_VIEWS) return false;
    return Runner_getCameraById(r, r->views[vc].cameraId) != nullptr;
}

static bool rdAppSurfaceOk(NetRenderDecoder* d) {
    return d->renderer->vtable->surfaceExists(d->renderer, d->runner->applicationSurfaceId);
}

static bool rdSizeOk(int32_t w, int32_t h) {
    return w >= 1 && w <= RD_MAX_SURFACE && h >= 1 && h <= RD_MAX_SURFACE;
}

// ===[ State ]===

static void rrDecState(NetRenderDecoder* d, NetReader* r) {
    Renderer* b = d->renderer;
    uint8_t mask = NetReader_u8(r);
    if (mask & 1) b->drawColor = NetReader_u32(r);
    if (mask & 2) b->drawAlpha = NetReader_f32(r);
    if (mask & 4) {
        int32_t f = NetReader_i32(r);
        b->drawFont = (f >= 0 && (uint32_t) f < d->dw->font.count) ? f : -1;
    }
    if (mask & 8) b->drawHalign = NetReader_i32(r);
    if (mask & 16) b->drawValign = NetReader_i32(r);
    if (mask & 32) b->circlePrecision = NetReader_i32(r);
    if (mask & 64) b->cameraCurrent = NetReader_i32(r);
}

static void rrDecRunnerState(NetRenderDecoder* d, NetReader* r) {
    Runner* runner = d->runner;
    uint8_t flags = NetReader_u8(r);
    if (flags & 1) {
        runner->viewsEnabled = NetReader_bool(r);
        int32_t vcur = NetReader_i32(r);
        runner->viewCurrent = (vcur >= 0 && vcur < MAX_VIEWS) ? vcur : 0;
        runner->usingAppSurface = NetReader_bool(r);
        runner->appSurfaceAutoDraw = NetReader_bool(r);
    }
    if (flags & 2) {
        uint32_t n = NetReader_u32(r);
        for (uint32_t i = 0; i < n && !r->error; i++) {
            uint8_t idx = NetReader_u8(r);
            RuntimeView tmp;
            tmp.enabled = NetReader_bool(r);
            tmp.portX = NetReader_i32(r);
            tmp.portY = NetReader_i32(r);
            tmp.portWidth = NetReader_i32(r);
            tmp.portHeight = NetReader_i32(r);
            tmp.cameraId = NetReader_i32(r);
            tmp.surfaceId = rdMapSurface(d, NetReader_i32(r));
            if (idx < MAX_VIEWS && !r->error) runner->views[idx] = tmp;
            if (getenv("BUTTERSCOTCH_NET_DEBUG") != nullptr) logInfo("Net-dbg client: view %u en=%d port=(%d,%d %dx%d) cam=%d surf=%d\n", (unsigned) idx, (int) tmp.enabled, tmp.portX, tmp.portY, tmp.portWidth, tmp.portHeight, tmp.cameraId, tmp.surfaceId);
        }
    }
    if (flags & 4) {
        uint32_t n = NetReader_u32(r);
        for (uint32_t i = 0; i < n && !r->error; i++) {
            uint32_t id = NetReader_u32(r);
            GMLCamera tmp;
            memset(&tmp, 0, sizeof(tmp));
            NetRender_readCamera(r, &tmp);
            if (id >= MAX_CAMERAS || r->error) continue;
            GMLCamera* dst = id < MAX_DEFAULT_ROOM_CAMERAS ? &runner->defaultCameras[id] : &runner->userCameras[id - MAX_DEFAULT_ROOM_CAMERAS];
            *dst = tmp;
            if (getenv("BUTTERSCOTCH_NET_DEBUG") != nullptr) logInfo("Net-dbg client: camera %u alloc=%d view=(%.1f,%.1f %dx%d) ang=%.1f\n", id, (int) tmp.allocated, tmp.viewX, tmp.viewY, tmp.viewWidth, tmp.viewHeight, tmp.viewAngle);
        }
    }
}

// ===[ Manual decoders ]===

static void rrDec_beginFrame(NetRenderDecoder* d, NetReader* r) {
    int32_t gameW = NetReader_i32(r), gameH = NetReader_i32(r);
    NetReader_i32(r); // server virtual window width – replaced by our real window
    NetReader_i32(r);
    if (r->error) return;
    // The renderer binds the application surface and divides by the game size
    if (gameW < 1 || gameW > RD_MAX_WINDOW || gameH < 1 || gameH > RD_MAX_WINDOW || !rdAppSurfaceOk(d)) return;
    d->renderer->vtable->beginFrame(d->renderer, gameW, gameH, d->winW, d->winH);
}

static void rrDec_beginGUI(NetRenderDecoder* d, NetReader* r) {
    int32_t guiW = NetReader_i32(r), guiH = NetReader_i32(r);
    int32_t portX = NetReader_i32(r), portY = NetReader_i32(r), portW = NetReader_i32(r), portH = NetReader_i32(r);
    int32_t serverTarget = NetReader_i32(r);
    if (r->error) return;
    if (guiW < 1 || guiW > RD_MAX_WINDOW || guiH < 1 || guiH > RD_MAX_WINDOW) return;
    int32_t target = rdMapSurface(d, serverTarget);
    // A target that doesn't exist (any more) must not reach the renderer, which treats that as a fatal error
    if (target >= 0 && !d->renderer->vtable->surfaceExists(d->renderer, target)) target = -1;
    if (target == RENDER_TARGET_HOST_FRAMEBUFFER) {
        // Addressing the window: the server used its virtual window size, we use ours (the GUI is letterboxed into it)
        portX = 0;
        portY = 0;
        portW = d->winW;
        portH = d->winH;
    } else if (target < 0) {
        // Target surface is unknown to us (late join) – draw to the window instead of aborting
        target = RENDER_TARGET_HOST_FRAMEBUFFER;
        portX = 0;
        portY = 0;
        portW = d->winW;
        portH = d->winH;
    }
    d->renderer->vtable->beginGUI(d->renderer, guiW, guiH, portX, portY, portW, portH, target);
}

static void rrDec_createSurface(NetRenderDecoder* d, NetReader* r) {
    int32_t serverId = NetReader_i32(r), w = NetReader_i32(r), h = NetReader_i32(r);
    if (r->error || !rdSizeOk(w, h) || serverId < 0 || (uint32_t) serverId >= RD_MAX_IDS) return;
    int32_t local = d->renderer->vtable->createSurface(d->renderer, w, h);
    mapSurface(d, serverId, local);
}

static void rrDec_ensureApplicationSurface(NetRenderDecoder* d, NetReader* r) {
    int32_t serverId = NetReader_i32(r), w = NetReader_i32(r), h = NetReader_i32(r);
    if (r->error || !rdSizeOk(w, h) || serverId < 0 || (uint32_t) serverId >= RD_MAX_IDS) return;
    int32_t local = d->renderer->vtable->ensureApplicationSurface(d->renderer, w, h);
    d->runner->applicationSurfaceId = local;
    mapSurface(d, serverId, local);
}

static void rrDec_setRenderTarget(NetRenderDecoder* d, NetReader* r) {
    int32_t surf = rdMapSurface(d, NetReader_i32(r));
    bool implicitApp = NetReader_bool(r);
    if (r->error || !rdViewOk(d)) return; // the renderer looks up the current view's camera
    d->renderer->vtable->setRenderTarget(d->renderer, surf, implicitApp);
}

static void rrDec_surfaceSetPixels(NetRenderDecoder* d, NetReader* r) {
    int32_t surf = rdMapSurface(d, NetReader_i32(r));
    uint32_t n = NetReader_u32(r);
    const uint8_t* px = NetReader_bytes(r, n);
    if (r->error || px == nullptr) return;
    RendererVtable* vt = d->renderer->vtable;
    if (vt->surfaceSetPixels == nullptr || !vt->surfaceExists(d->renderer, surf)) return;
    // The renderer reads width*height*4 bytes: the payload must be exactly that
    uint64_t expected = (uint64_t) vt->getSurfaceWidth(d->renderer, surf) * (uint64_t) vt->getSurfaceHeight(d->renderer, surf) * 4u;
    if (expected != (uint64_t) n) return;
    vt->surfaceSetPixels(d->renderer, surf, px);
}

static void rrDec_surfaceUploadPixels(NetRenderDecoder* d, NetReader* r) {
    int32_t surf = rdMapSurface(d, NetReader_i32(r));
    int32_t w = NetReader_i32(r), h = NetReader_i32(r);
    if (!rdSizeOk(w, h) || r->error) { r->error = true; return; }
    const uint8_t* px = NetReader_bytes(r, (size_t) w * (size_t) h * 4u);
    if (r->error || px == nullptr) return;
    RendererVtable* vt = d->renderer->vtable;
    if (vt->surfaceUploadPixels != nullptr && vt->surfaceExists(d->renderer, surf)) vt->surfaceUploadPixels(d->renderer, surf, w, h, px);
}

static void rrDec_createSpriteFromSurface(NetRenderDecoder* d, NetReader* r) {
    int32_t surf = rdMapSurface(d, NetReader_i32(r));
    int32_t x = NetReader_i32(r), y = NetReader_i32(r), w = NetReader_i32(r), h = NetReader_i32(r);
    bool removeback = NetReader_bool(r), smooth = NetReader_bool(r);
    int32_t xorig = NetReader_i32(r), yorig = NetReader_i32(r);
    uint32_t serverTpag = NetReader_u32(r), serverSprite = NetReader_u32(r);
    if (r->error || !rdSizeOk(w, h) || surf < 0) return;
    if (serverTpag > d->origTpag + RD_MAX_IDS || serverSprite > d->origSprite + RD_MAX_IDS) return;
    int32_t local = d->renderer->vtable->createSpriteFromSurface(d->renderer, surf, x, y, w, h, removeback, smooth, xorig, yorig);
    int32_t localTpag = -1;
    if (local >= 0 && (uint32_t) local < d->dw->sprt.count && d->dw->sprt.sprites[local].textureCount > 0) {
        localTpag = d->dw->sprt.sprites[local].tpagIndices[0];
    }
    if (serverTpag >= d->origTpag && growInts(&d->tpagMap, &d->tpagMapCount, serverTpag - d->origTpag + 1)) {
        d->tpagMap[serverTpag - d->origTpag] = localTpag;
    }
    if (serverSprite >= d->origSprite && growInts(&d->spriteMap, &d->spriteMapCount, serverSprite - d->origSprite + 1)) {
        d->spriteMap[serverSprite - d->origSprite] = local;
    }
}

static void rrDec_drawTile(NetRenderDecoder* d, NetReader* r) {
    RoomTile t;
    memset(&t, 0, sizeof(t));
    t.x = NetReader_i32(r);
    t.y = NetReader_i32(r);
    t.useSpriteDefinition = NetReader_bool(r);
    t.backgroundDefinition = NetReader_i32(r);
    t.sourceX = NetReader_i32(r);
    t.sourceY = NetReader_i32(r);
    t.width = NetReader_u32(r);
    t.height = NetReader_u32(r);
    t.tileDepth = NetReader_i32(r);
    t.instanceID = NetReader_u32(r);
    t.scaleX = NetReader_f32(r);
    t.scaleY = NetReader_f32(r);
    t.color = NetReader_u32(r);
    t.alpha = NetReader_f32(r);
    float ox = NetReader_f32(r), oy = NetReader_f32(r);
    if (r->error) return;
    if (d->renderer->vtable->drawTile != nullptr) d->renderer->vtable->drawTile(d->renderer, &t, ox, oy);
}

static void rrDec_drawVertexBuffer(NetRenderDecoder* d, NetReader* r) {
    int32_t primitive = NetReader_i32(r);
    uint32_t texture = rdMapTexture(d, NetReader_u32(r));
    int32_t number = NetReader_i32(r);
    uint32_t stride = NetReader_u32(r);
    uint32_t numElements = NetReader_u32(r);
    if (r->error || numElements > 16 || stride == 0 || stride > 256 || number <= 0 || number > (1 << 22)) { r->error = true; return; }
    VertexFormat* f = &d->vf;
    memset(f, 0, sizeof(*f));
    f->numElements = (int) numElements;
    f->stride = stride;
    for (uint32_t i = 0; i < numElements; i++) {
        int32_t usage = NetReader_i32(r), type = NetReader_i32(r);
        uint32_t off = NetReader_u32(r), size = NetReader_u32(r);
        // The renderer points the GPU at (offset, type) inside each vertex: it has to stay within one vertex
        if (usage < VERTEX_USAGE_POSITION || usage > VERTEX_USAGE_TEXCOORD || type < VERTEX_TYPE_FLOAT1 || type > VERTEX_TYPE_COLOR ||
            off > stride || size > stride || off + size > stride) {
            r->error = true;
            return;
        }
        f->elements[i].usage = (VertexUsage) usage;
        f->elements[i].type = (VertexType) type;
        f->elements[i].offset = off;
        f->elements[i].size = size;
    }
    // Element offset + the size of its type must also fit (position / texcoord / normal are read as floats, colour as 4 bytes)
    for (uint32_t i = 0; i < numElements; i++) {
        uint32_t need = 0;
        switch (f->elements[i].usage) {
            case VERTEX_USAGE_POSITION: need = 3 * 4; break;   // up to 3 floats
            case VERTEX_USAGE_COLOR: need = 4; break;
            case VERTEX_USAGE_NORMAL: need = 3 * 4; break;
            case VERTEX_USAGE_TEXCOORD: need = 2 * 4; break;
        }
        if (f->elements[i].offset + need > stride) { r->error = true; return; }
    }
    size_t bytes = (size_t) number * stride;
    const uint8_t* data = NetReader_bytes(r, bytes);
    if (r->error || data == nullptr) return;
    VertexBuffer* vb = &d->vb;
    vb->data = (uint8_t*) data;
    vb->size = bytes;
    vb->capacity = bytes;
    vb->format = f;
    vb->vertexSize = stride;
    vb->isFrozen = false;
    if (d->renderer->vtable->drawVertexBuffer != nullptr) {
        d->renderer->vtable->drawVertexBuffer(d->renderer, vb, primitive, (int32_t) texture, 0, number);
    }
    vb->data = nullptr;
}

static void defineHandle(NetRenderDecoder* d, NetReader* r, bool sampler) {
    int32_t id = NetReader_i32(r);
    int32_t shader = NetReader_i32(r);
    char* name = NetReader_strDup(r);
    if (r->error || name == nullptr || id < NET_VIRTUAL_HANDLE_BASE || (uint32_t) (id - NET_VIRTUAL_HANDLE_BASE) >= RD_MAX_IDS ||
        shader < 0 || (uint32_t) shader >= d->dw->shdr.count || strlen(name) > 256) {
        free(name);
        r->error = true;
        return;
    }
    int32_t local = -1;
    RendererVtable* vt = d->renderer->vtable;
    if (sampler) local = vt->shaderGetSamplerIndex(d->renderer, shader, name);
    else local = vt->shaderGetUniform(d->renderer, shader, name);
    if (growInts(&d->uniMap, &d->uniMapCount, (uint32_t) (id - NET_VIRTUAL_HANDLE_BASE) + 1)) d->uniMap[id - NET_VIRTUAL_HANDLE_BASE] = local;
    free(name);
}

static void rrDec_shaderGetUniform(NetRenderDecoder* d, NetReader* r) { defineHandle(d, r, false); }
static void rrDec_shaderGetSamplerIndex(NetRenderDecoder* d, NetReader* r) { defineHandle(d, r, true); }

static void rrDec_shaderSetUniformFArray(NetRenderDecoder* d, NetReader* r) {
    int32_t handle = rdMapUniform(d, NetReader_i32(r));
    uint32_t count = NetReader_u32(r);
    if (r->error || count > (1u << 20)) { r->error = true; return; }
    float* values = (float*) safeMalloc(count * sizeof(float));
    for (uint32_t i = 0; i < count; i++) values[i] = NetReader_f32(r);
    if (!r->error && d->renderer->vtable->shaderSetUniformFArray != nullptr) {
        d->renderer->vtable->shaderSetUniformFArray(d->renderer, handle, values, count);
    }
    free(values);
}

#include "net_render_gen_decl.inc"
#include "net_render_gen.inc"

// ===[ Public API ]===

NetRenderDecoder* NetRenderDecoder_create(Renderer* real, Runner* clientRunner) {
    NetRenderDecoder* d = (NetRenderDecoder*) safeCalloc(1, sizeof(NetRenderDecoder));
    d->renderer = real;
    d->runner = clientRunner;
    d->dw = real->dataWin;
    d->origTpag = d->dw->tpag.count;
    d->origSprite = d->dw->sprt.count;
    d->winW = 640;
    d->winH = 480;
    return d;
}

void NetRenderDecoder_destroy(NetRenderDecoder* d) {
    if (d == nullptr) return;
    free(d->surfMap);
    free(d->tpagMap);
    free(d->spriteMap);
    free(d->uniMap);
    free(d);
}

void NetRenderDecoder_setWindowSize(NetRenderDecoder* d, int32_t w, int32_t h) {
    d->winW = w;
    d->winH = h;
}

// FNV-1a over pixel data (snapshot self-check)
static uint32_t hashBytes(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

bool NetRenderDecoder_buildSnapshot(NetRenderDecoder* d, const uint8_t* request, size_t len, NetWriter* out) {
    NetReader r;
    NetReader_init(&r, request, len);
    uint32_t id = NetReader_u32(&r);
    uint32_t count = NetReader_u32(&r);
    if (r.error || count > 4096) return false;
    NetWriter_u32(out, id);
    NetWriter countPos;
    NetWriter_init(&countPos);
    NetWriter body;
    NetWriter_init(&body);
    uint32_t written = 0;
    RendererVtable* vt = d->renderer->vtable;
    for (uint32_t i = 0; i < count; i++) {
        int32_t serverId = NetReader_i32(&r);
        if (r.error) break;
        int32_t local = rdMapSurface(d, serverId);
        if (local < 0 || !vt->surfaceExists(d->renderer, local) || vt->surfaceGetPixels == nullptr) continue;
        int32_t w = (int32_t) vt->getSurfaceWidth(d->renderer, local);
        int32_t h = (int32_t) vt->getSurfaceHeight(d->renderer, local);
        if (w <= 0 || h <= 0 || (int64_t) w * h > 16384 * 16384) continue;
        size_t bytes = (size_t) w * (size_t) h * 4u;
        uint8_t* px = (uint8_t*) safeMalloc(bytes);
        if (!vt->surfaceGetPixels(d->renderer, local, px)) { free(px); continue; }
        bool blank = true;
        for (size_t k = 0; k < bytes; k++) if (px[k] != 0) { blank = false; break; }
        NetWriter_i32(&body, serverId);
        NetWriter_i32(&body, w);
        NetWriter_i32(&body, h);
        NetWriter_bool(&body, !blank);
        if (!blank) {
            NetWriter_fixed32(&body, hashBytes(px, bytes));
            NetWriter_bytes(&body, px, bytes);
        }
        free(px);
        written++;
    }
    NetWriter_u32(out, written);
    NetWriter_bytes(out, body.data, body.len);
    NetWriter_free(&body);
    NetWriter_free(&countPos);
    return true;
}

int NetRenderDecoder_applySnapshot(NetRenderDecoder* d, const uint8_t* data, size_t len, bool verify) {
    NetReader r;
    NetReader_init(&r, data, len);
    NetReader_u32(&r); // request id
    uint32_t count = NetReader_u32(&r);
    int restored = 0;
    RendererVtable* vt = d->renderer->vtable;
    for (uint32_t i = 0; i < count && !r.error; i++) {
        int32_t serverId = NetReader_i32(&r);
        int32_t w = NetReader_i32(&r), h = NetReader_i32(&r);
        bool hasData = NetReader_bool(&r);
        if (r.error || !rdSizeOk(w, h)) break;
        if (!hasData) continue; // blank surfaces are already blank
        uint32_t hash = NetReader_fixed32(&r);
        size_t bytes = (size_t) w * (size_t) h * 4u;
        const uint8_t* px = NetReader_bytes(&r, bytes);
        if (r.error || px == nullptr) break;
        int32_t local = rdMapSurface(d, serverId);
        if (local < 0 || !vt->surfaceExists(d->renderer, local) || vt->surfaceSetPixels == nullptr) continue;
        if ((int32_t) vt->getSurfaceWidth(d->renderer, local) != w || (int32_t) vt->getSurfaceHeight(d->renderer, local) != h) continue;
        // surfaceSetPixels takes top-down RGBA of the full surface, the exact inverse of surfaceGetPixels
        vt->surfaceSetPixels(d->renderer, local, px);
        restored++;
        if (verify && vt->surfaceGetPixels != nullptr) {
            uint8_t* back = (uint8_t*) safeMalloc(bytes);
            bool ok = vt->surfaceGetPixels(d->renderer, local, back) && hashBytes(back, bytes) == hash;
            logInfo("Net: snapshot of surface %d (%dx%d) %s\n", serverId, w, h, ok ? "verified" : "MISMATCH after upload");
            free(back);
        }
    }
    return restored;
}

void NetRenderDecoder_setFrameBeginHook(NetRenderDecoder* d, void (*hook)(void*), void* userData) {
    d->frameBeginHook = hook;
    d->frameBeginData = userData;
}

bool NetRenderDecoder_run(NetRenderDecoder* d, const uint8_t* data, size_t len, bool* outFrameEnd) {
    NetReader r;
    NetReader_init(&r, data, len);
    *outFrameEnd = false;
    while (!NetReader_eof(&r)) {
        uint8_t op = NetReader_u8(&r);
        if (op >= NROP__COUNT) {
            logError("Net: unknown renderer opcode %u\n", (unsigned) op);
            return false;
        }
        if (op == NROP_FRAME_BEGIN) {
            if (d->frameBeginHook != nullptr) d->frameBeginHook(d->frameBeginData);
            continue;
        }
        if (op == NROP_FRAME_END) {
            *outFrameEnd = true;
            continue;
        }
        RrDecodeFn fn = rrDecoders[op];
        if (fn == nullptr) return false;
        fn(d, &r);
        if (r.error) {
            logError("Net: malformed renderer command '%s'\n", rrOpNames[op]);
            return false;
        }
    }
    return true;
}
