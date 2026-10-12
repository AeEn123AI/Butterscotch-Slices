// Server-side recording renderer. See net_render.h.
#define NET_RENDER_SERVER

#include "net_render.h"
#include "net_render_common.h"
#include "net_render_ops.h"

#include "data_win.h"
#include "log.h"
#include "utils.h"
#include "stb_ds.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int32_t w, h; bool exists; } RrSurface;
typedef struct { int32_t shader; char* name; int32_t id; bool sampler; } RrUniform;
typedef struct { uint8_t len; bool live; uint8_t data[255]; } RrBlob; // last-sent serialized form of a view / camera slot

#define RR_MAX_BLOB 255

typedef struct RemoteRenderer {
    Renderer base; // must be first
    NetWriter w;
    uint64_t callCount;
    bool shadersSupported;
    bool initialTexFilter;

    // Last-sent base state
    bool baseValid;
    uint32_t sColor;
    float sAlpha;
    int32_t sFont, sHalign, sValign, sCircle, sCamera;

    // Last-sent runner state
    bool runnerValid;
    bool sViewsEnabled, sUsingAppSurface, sAppSurfaceAutoDraw;
    int32_t sViewCurrent;
    RrBlob viewBlob[MAX_VIEWS];
    RrBlob camBlob[MAX_CAMERAS];

    // Shadow GPU state
    bool blendEnable;
    int32_t blendMode;
    BlendFactors blendFactors;
    bool alphaTestEnable;
    uint8_t alphaTestRef;
    bool cwR, cwG, cwB, cwA;

    // Surfaces (IDs are never reused, exactly like the GL renderers)
    RrSurface* surfaces;
    uint32_t surfaceCount;

    // Dynamic sprites
    uint32_t origTpagCount, origSpriteCount;
    int32_t* dynDims; // pairs (w,h) per dynamic tpag slot (index - origTpagCount)
    uint32_t dynTpagSlots;

    // Cached texture page dimensions (index = texture page). 0 = unknown
    int32_t* pageDims;
    uint32_t pageCount;

    // Virtual shader handles
    RrUniform* uniforms; // stb_ds
    int32_t nextHandle;
} RemoteRenderer;

// ===[ Helpers ]===

static inline NetWriter* rrWriter(RemoteRenderer* rr) { return &rr->w; }

static void rrSyncBase(RemoteRenderer* rr) {
    Renderer* b = &rr->base;
    uint32_t mask = 0;
    if (!rr->baseValid || rr->sColor != b->drawColor) mask |= 1;
    if (!rr->baseValid || rr->sAlpha != b->drawAlpha) mask |= 2;
    if (!rr->baseValid || rr->sFont != b->drawFont) mask |= 4;
    if (!rr->baseValid || rr->sHalign != b->drawHalign) mask |= 8;
    if (!rr->baseValid || rr->sValign != b->drawValign) mask |= 16;
    if (!rr->baseValid || rr->sCircle != b->circlePrecision) mask |= 32;
    if (!rr->baseValid || rr->sCamera != b->cameraCurrent) mask |= 64;
    if (mask == 0) return;
    NetWriter* w = &rr->w;
    NetWriter_u8(w, NROP_STATE);
    NetWriter_u8(w, (uint8_t) mask);
    if (mask & 1) NetWriter_u32(w, b->drawColor);
    if (mask & 2) NetWriter_f32(w, b->drawAlpha);
    if (mask & 4) NetWriter_i32(w, b->drawFont);
    if (mask & 8) NetWriter_i32(w, b->drawHalign);
    if (mask & 16) NetWriter_i32(w, b->drawValign);
    if (mask & 32) NetWriter_i32(w, b->circlePrecision);
    if (mask & 64) NetWriter_i32(w, b->cameraCurrent);
    rr->sColor = b->drawColor;
    rr->sAlpha = b->drawAlpha;
    rr->sFont = b->drawFont;
    rr->sHalign = b->drawHalign;
    rr->sValign = b->drawValign;
    rr->sCircle = b->circlePrecision;
    rr->sCamera = b->cameraCurrent;
    rr->baseValid = true;
}

// Compares `fresh` against the last-sent blob; returns true (and updates the cache) when it differs.
static bool rrBlobChanged(RrBlob* cache, const NetWriter* fresh) {
    if (fresh->len > RR_MAX_BLOB) {
        // Cannot be cached: always send
        cache->len = 0;
        return true;
    }
    if (cache->len == fresh->len && memcmp(cache->data, fresh->data, fresh->len) == 0) return false;
    cache->len = (uint8_t) fresh->len;
    memcpy(cache->data, fresh->data, fresh->len);
    return true;
}

static void rrSyncRunner(RemoteRenderer* rr) {
    Runner* runner = rr->base.runner;
    if (runner == nullptr) return;

    NetWriter body;
    NetWriter_init(&body);
    uint8_t flags = 0;

    // Scalars
    if (!rr->runnerValid || rr->sViewsEnabled != runner->viewsEnabled || rr->sViewCurrent != runner->viewCurrent ||
        rr->sUsingAppSurface != runner->usingAppSurface || rr->sAppSurfaceAutoDraw != runner->appSurfaceAutoDraw) {
        flags |= 1;
        NetWriter_bool(&body, runner->viewsEnabled);
        NetWriter_i32(&body, runner->viewCurrent);
        NetWriter_bool(&body, runner->usingAppSurface);
        NetWriter_bool(&body, runner->appSurfaceAutoDraw);
        rr->sViewsEnabled = runner->viewsEnabled;
        rr->sViewCurrent = runner->viewCurrent;
        rr->sUsingAppSurface = runner->usingAppSurface;
        rr->sAppSurfaceAutoDraw = runner->appSurfaceAutoDraw;
    }

    // Views
    NetWriter tmp;
    NetWriter_init(&tmp);
    NetWriter viewsOut;
    NetWriter_init(&viewsOut);
    uint32_t viewCount = 0;
    for (int i = 0; i < MAX_VIEWS; i++) {
        RuntimeView* v = &runner->views[i];
        NetWriter_reset(&tmp);
        NetWriter_bool(&tmp, v->enabled);
        NetWriter_i32(&tmp, v->portX);
        NetWriter_i32(&tmp, v->portY);
        NetWriter_i32(&tmp, v->portWidth);
        NetWriter_i32(&tmp, v->portHeight);
        NetWriter_i32(&tmp, v->cameraId);
        NetWriter_i32(&tmp, v->surfaceId);
        if (rrBlobChanged(&rr->viewBlob[i], &tmp)) {
            NetWriter_u8(&viewsOut, (uint8_t) i);
            NetWriter_bytes(&viewsOut, tmp.data, tmp.len);
            viewCount++;
        }
    }
    if (viewCount > 0) {
        flags |= 2;
        NetWriter_u32(&body, viewCount);
        NetWriter_bytes(&body, viewsOut.data, viewsOut.len);
    }

    // Cameras (default room cameras and user cameras; the GUI / surface cameras are derived by the client's renderer itself)
    NetWriter camsOut;
    NetWriter_init(&camsOut);
    uint32_t camCount = 0;
    for (int id = 0; id < MAX_CAMERAS; id++) {
        GMLCamera* c = Runner_getCameraById(runner, id);
        RrBlob* cache = &rr->camBlob[id];
        if (c == nullptr) {
            // Slot is free: tell the client once if it was allocated before
            if (!cache->live) continue;
            NetWriter_u32(&camsOut, (uint32_t) id);
            NetWriter_bool(&camsOut, false);
            cache->live = false;
            cache->len = 0;
            camCount++;
            continue;
        }
        NetWriter_reset(&tmp);
        NetRender_writeCamera(&tmp, c);
        if (rrBlobChanged(cache, &tmp)) {
            NetWriter_u32(&camsOut, (uint32_t) id);
            NetWriter_bytes(&camsOut, tmp.data, tmp.len);
            if (getenv("BUTTERSCOTCH_NET_DEBUG") != nullptr) logInfo("Net-dbg server: camera %d alloc=%d view=(%.1f,%.1f %dx%d) ang=%.1f\n", id, (int) c->allocated, c->viewX, c->viewY, c->viewWidth, c->viewHeight, c->viewAngle);
            cache->live = true;
            camCount++;
        }
    }
    if (camCount > 0) {
        flags |= 4;
        NetWriter_u32(&body, camCount);
        NetWriter_bytes(&body, camsOut.data, camsOut.len);
    }

    rr->runnerValid = true;
    if (flags != 0) {
        NetWriter_u8(&rr->w, NROP_RSTATE);
        NetWriter_u8(&rr->w, flags);
        NetWriter_bytes(&rr->w, body.data, body.len);
    }
    NetWriter_free(&body);
    NetWriter_free(&tmp);
    NetWriter_free(&viewsOut);
    NetWriter_free(&camsOut);
}

static NetWriter* rrBegin(RemoteRenderer* rr, int op, bool runnerSync) {
    rr->callCount++;
    rrSyncBase(rr);
    if (runnerSync) rrSyncRunner(rr);
    NetWriter_u8(&rr->w, (uint8_t) op);
    return &rr->w;
}

static inline bool rrSurfaceValid(RemoteRenderer* rr, int32_t id) {
    return id >= 0 && (uint32_t) id < rr->surfaceCount && rr->surfaces[id].exists;
}

// ===[ Generated encoders ]===
// Forward declarations of the shadow hooks used by generated code
static void rrShadow_gpuSetBlendMode(RemoteRenderer* rr, int32_t mode);
static void rrShadow_gpuSetBlendModeExt(RemoteRenderer* rr, int32_t sfactor, int32_t dfactor, int32_t sfactor_alpha, int32_t dfactor_alpha);
static void rrShadow_gpuSetBlendEnable(RemoteRenderer* rr, bool enable);
static void rrShadow_gpuSetAlphaTestEnable(RemoteRenderer* rr, bool enable);
static void rrShadow_gpuSetAlphaTestRef(RemoteRenderer* rr, uint8_t ref);
static void rrShadow_gpuSetColorWriteEnable(RemoteRenderer* rr, bool red, bool green, bool blue, bool alpha);
static void rrShadow_gpuSetTexFilter(RemoteRenderer* rr, bool enable);
static void rrShadow_gpuSetShader(RemoteRenderer* rr, int32_t shaderIndex);
static void rrShadow_gpuResetShader(RemoteRenderer* rr);
static void rrShadow_setMatrix(RemoteRenderer* rr, int32_t matrixType, Matrix4f matrix);
static void rrShadow_deleteSprite(RemoteRenderer* rr, int32_t spriteIndex);
static void rrShadow_surfaceResize(RemoteRenderer* rr, int32_t surfaceID, int32_t width, int32_t height);
static void rrShadow_surfaceFree(RemoteRenderer* rr, int32_t surfaceID);

#include "net_render_gen.inc"

// ===[ Shadow state hooks ]===

// Same factor tables the GL renderers use (values are OpenGL enums)
static int32_t rrBlendModeS(int32_t mode) {
    switch (mode) {
        case bm_subtract: return 0;          // GL_ZERO
        case bm_min: return 1;               // GL_ONE
        default: return 0x0302;              // GL_SRC_ALPHA
    }
}
static int32_t rrBlendModeD(int32_t mode) {
    switch (mode) {
        case bm_add: case bm_reverse_subtract: case bm_min: return 1;   // GL_ONE
        case bm_subtract: case bm_max: return 0x0301;                  // GL_ONE_MINUS_SRC_COLOR
        default: return 0x0303;                                         // GL_ONE_MINUS_SRC_ALPHA
    }
}
static void rrShadow_gpuSetBlendMode(RemoteRenderer* rr, int32_t mode) {
    rr->blendMode = mode;
    rr->blendFactors.src = rr->blendFactors.srcAlpha = rrBlendModeS(mode);
    rr->blendFactors.dst = rr->blendFactors.dstAlpha = rrBlendModeD(mode);
}
static void rrShadow_gpuSetBlendModeExt(RemoteRenderer* rr, int32_t sf, int32_t df, int32_t sfa, int32_t dfa) {
    rr->blendMode = bm_complex;
    rr->blendFactors.src = sf;
    rr->blendFactors.dst = df;
    rr->blendFactors.srcAlpha = sfa;
    rr->blendFactors.dstAlpha = dfa;
}
static void rrShadow_gpuSetBlendEnable(RemoteRenderer* rr, bool enable) { rr->blendEnable = enable; }
static void rrShadow_gpuSetAlphaTestEnable(RemoteRenderer* rr, bool enable) { rr->alphaTestEnable = enable; }
static void rrShadow_gpuSetAlphaTestRef(RemoteRenderer* rr, uint8_t ref) { rr->alphaTestRef = ref; }
static void rrShadow_gpuSetColorWriteEnable(RemoteRenderer* rr, bool r, bool g, bool b, bool a) {
    rr->cwR = r; rr->cwG = g; rr->cwB = b; rr->cwA = a;
}
static void rrShadow_gpuSetTexFilter(RemoteRenderer* rr, bool enable) { rr->base.texFilter = enable; }
static void rrShadow_gpuSetShader(RemoteRenderer* rr, int32_t shaderIndex) { rr->base.currentShader = shaderIndex; }
static void rrShadow_gpuResetShader(RemoteRenderer* rr) { rr->base.currentShader = -1; }

// Mirrors the GL renderers: derived matrices are recomputed whenever a matrix changes
static void rrShadow_setMatrix(RemoteRenderer* rr, int32_t matrixType, Matrix4f matrix) {
    Renderer* r = &rr->base;
    if (matrixType < 0 || matrixType >= MATRICES_MAX) return;
    if (memcmp(&r->gmlMatrices[matrixType], &matrix, sizeof(Matrix4f)) == 0) return;
    r->gmlMatrices[matrixType] = matrix;
    Matrix4f worldView, wvp;
    Matrix4f_multiply(&worldView, &r->gmlMatrices[MATRIX_VIEW], &r->gmlMatrices[MATRIX_WORLD]);
    Matrix4f_multiply(&wvp, &r->gmlMatrices[MATRIX_PROJECTION], &worldView);
    r->gmlMatrices[MATRIX_WORLD_VIEW] = worldView;
    r->gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION] = wvp;
}

static void rrShadow_deleteSprite(RemoteRenderer* rr, int32_t spriteIndex) {
    DataWin* dw = rr->base.dataWin;
    if (0 > spriteIndex || dw->sprt.count <= (uint32_t) spriteIndex) return;
    if (rr->origSpriteCount > (uint32_t) spriteIndex) return;
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return;
    repeat(sprite->textureCount, i) {
        int32_t tpagIdx = sprite->tpagIndices[i];
        if (tpagIdx >= 0 && (uint32_t) tpagIdx >= rr->origTpagCount) {
            dw->tpag.items[tpagIdx].texturePageId = -1;
        }
    }
    free(sprite->tpagIndices);
    const char* keepName = sprite->name;
    memset(sprite, 0, sizeof(Sprite));
    sprite->name = keepName;
}

static void rrShadow_surfaceResize(RemoteRenderer* rr, int32_t id, int32_t w, int32_t h) {
    if (!rrSurfaceValid(rr, id)) return;
    rr->surfaces[id].w = w;
    rr->surfaces[id].h = h;
}

static void rrShadow_surfaceFree(RemoteRenderer* rr, int32_t id) {
    if (id < 0 || (uint32_t) id >= rr->surfaceCount) return;
    // Freeing the application surface from GML is a no-op
    if (id == rr->base.runner->applicationSurfaceId) return;
    rr->surfaces[id].exists = false;
    rr->surfaces[id].w = rr->surfaces[id].h = 0;
}

// ===[ Manual encoders ]===

static int32_t rrNewSurface(RemoteRenderer* rr, int32_t w, int32_t h) {
    uint32_t id = rr->surfaceCount++;
    rr->surfaces = (RrSurface*) safeRealloc(rr->surfaces, rr->surfaceCount * sizeof(RrSurface));
    rr->surfaces[id].w = w;
    rr->surfaces[id].h = h;
    rr->surfaces[id].exists = true;
    return (int32_t) id;
}

static void rrEnc_beginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    NetWriter* w = rrBegin(rr, NROP_beginFrame, true);
    NetWriter_i32(w, gameW);
    NetWriter_i32(w, gameH);
    NetWriter_i32(w, windowW);
    NetWriter_i32(w, windowH);
}

static void rrEnc_beginGUI(Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, int32_t targetSurfaceId) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    NetWriter* w = rrBegin(rr, NROP_beginGUI, true);
    NetWriter_i32(w, guiW);
    NetWriter_i32(w, guiH);
    NetWriter_i32(w, portX);
    NetWriter_i32(w, portY);
    NetWriter_i32(w, portW);
    NetWriter_i32(w, portH);
    NetWriter_i32(w, targetSurfaceId);
}

static int32_t rrEnc_createSurface(Renderer* renderer, int32_t width, int32_t height) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    int32_t id = rrNewSurface(rr, width, height);
    NetWriter* w = rrBegin(rr, NROP_createSurface, false);
    NetWriter_i32(w, id);
    NetWriter_i32(w, width);
    NetWriter_i32(w, height);
    return id;
}

static int32_t rrEnc_ensureApplicationSurface(Renderer* renderer, int32_t width, int32_t height) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    Runner* runner = renderer->runner;
    int32_t id = runner->applicationSurfaceId;
    bool needsCreate = !rrSurfaceValid(rr, id);
    if (needsCreate) {
        id = rrNewSurface(rr, width, height);
        runner->applicationSurfaceId = id;
    } else if (rr->surfaces[id].w != width || rr->surfaces[id].h != height) {
        rr->surfaces[id].w = width;
        rr->surfaces[id].h = height;
    }
    NetWriter* w = rrBegin(rr, NROP_ensureApplicationSurface, true);
    NetWriter_i32(w, id);
    NetWriter_i32(w, width);
    NetWriter_i32(w, height);
    return id;
}

static bool rrEnc_setRenderTarget(Renderer* renderer, int32_t surfaceID, bool implicitApplicationSurface) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    if (!rrSurfaceValid(rr, surfaceID)) return false;
    NetWriter* w = rrBegin(rr, NROP_setRenderTarget, true);
    NetWriter_i32(w, surfaceID);
    NetWriter_bool(w, implicitApplicationSurface);
    return true;
}

static bool rrEnc_surfaceExists(Renderer* renderer, int32_t surfaceID) {
    return rrSurfaceValid((RemoteRenderer*) renderer, surfaceID);
}

static float rrEnc_getSurfaceWidth(Renderer* renderer, int32_t surfaceID) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    return rrSurfaceValid(rr, surfaceID) ? (float) rr->surfaces[surfaceID].w : 0.0f;
}

static float rrEnc_getSurfaceHeight(Renderer* renderer, int32_t surfaceID) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    return rrSurfaceValid(rr, surfaceID) ? (float) rr->surfaces[surfaceID].h : 0.0f;
}

// Pixel read-back needs the GPU, which lives on the clients. Reported as unsupported (callers handle `false`).
static bool rrEnc_surfaceGetPixels(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t surfaceID, MAYBE_UNUSED uint8_t* outRGBA) {
    return false;
}

static bool rrEnc_surfaceSetPixels(Renderer* renderer, int32_t surfaceID, const uint8_t* rgba) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    if (!rrSurfaceValid(rr, surfaceID) || rgba == nullptr) return false;
    NetWriter* w = rrBegin(rr, NROP_surfaceSetPixels, false);
    NetWriter_i32(w, surfaceID);
    NetWriter_u32(w, (uint32_t) rr->surfaces[surfaceID].w * (uint32_t) rr->surfaces[surfaceID].h * 4u);
    NetWriter_bytes(w, rgba, (size_t) rr->surfaces[surfaceID].w * (size_t) rr->surfaces[surfaceID].h * 4u);
    return true;
}

static void rrEnc_surfaceUploadPixels(Renderer* renderer, int32_t surfaceID, int32_t width, int32_t height, const uint8_t* rgba) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    if (!rrSurfaceValid(rr, surfaceID) || rgba == nullptr || width <= 0 || height <= 0) return;
    NetWriter* w = rrBegin(rr, NROP_surfaceUploadPixels, false);
    NetWriter_i32(w, surfaceID);
    NetWriter_i32(w, width);
    NetWriter_i32(w, height);
    NetWriter_bytes(w, rgba, (size_t) width * (size_t) height * 4u);
}

static void rrEnc_drawTile(Renderer* renderer, RoomTile* tile, float offsetX, float offsetY) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    NetWriter* w = rrBegin(rr, NROP_drawTile, false);
    NetWriter_i32(w, tile->x);
    NetWriter_i32(w, tile->y);
    NetWriter_bool(w, tile->useSpriteDefinition);
    NetWriter_i32(w, tile->backgroundDefinition);
    NetWriter_i32(w, tile->sourceX);
    NetWriter_i32(w, tile->sourceY);
    NetWriter_u32(w, tile->width);
    NetWriter_u32(w, tile->height);
    NetWriter_i32(w, tile->tileDepth);
    NetWriter_u32(w, tile->instanceID);
    NetWriter_f32(w, tile->scaleX);
    NetWriter_f32(w, tile->scaleY);
    NetWriter_u32(w, tile->color);
    NetWriter_f32(w, tile->alpha);
    NetWriter_f32(w, offsetX);
    NetWriter_f32(w, offsetY);
}

static void rrEnc_drawVertexBuffer(Renderer* renderer, VertexBuffer* buffer, int32_t primitive, int32_t texture, int32_t offset, int32_t number) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    if (buffer == nullptr || buffer->format == nullptr || buffer->format->stride == 0 || buffer->data == nullptr) return;
    int32_t vertexCount = (int32_t) (buffer->size / buffer->format->stride);
    if (vertexCount <= 0) return;
    if (offset < 0) offset = 0;
    if (offset > vertexCount) offset = vertexCount;
    if (number == -1) number = vertexCount - offset;
    else {
        if (number < 0) number = 0;
        if (offset + number > vertexCount) number = vertexCount - offset;
    }
    if (number <= 0) return;
    NetWriter* w = rrBegin(rr, NROP_drawVertexBuffer, false);
    NetWriter_i32(w, primitive);
    NetWriter_i32(w, texture);
    NetWriter_i32(w, number);
    VertexFormat* f = buffer->format;
    NetWriter_u32(w, f->stride);
    NetWriter_u32(w, (uint32_t) f->numElements);
    for (int i = 0; i < f->numElements; i++) {
        NetWriter_i32(w, (int32_t) f->elements[i].usage);
        NetWriter_i32(w, (int32_t) f->elements[i].type);
        NetWriter_u32(w, f->elements[i].offset);
        NetWriter_u32(w, f->elements[i].size);
    }
    NetWriter_bytes(w, buffer->data + (size_t) offset * f->stride, (size_t) number * f->stride);
}

// ---- Dynamic sprites (mirrors glCreateSpriteFromSurface so server and client allocate identical slots) ----

static uint32_t rrFindOrAllocTpagSlot(RemoteRenderer* rr) {
    DataWin* dw = rr->base.dataWin;
    for (uint32_t i = rr->origTpagCount; dw->tpag.count > i; i++) {
        if (dw->tpag.items[i].texturePageId == -1) return i;
    }
    uint32_t newIndex = dw->tpag.count;
    dw->tpag.count++;
    dw->tpag.items = (TexturePageItem*) safeRealloc(dw->tpag.items, dw->tpag.count * sizeof(TexturePageItem));
    memset(&dw->tpag.items[newIndex], 0, sizeof(TexturePageItem));
    dw->tpag.items[newIndex].texturePageId = -1;
    return newIndex;
}

static int32_t rrEnc_createSpriteFromSurface(Renderer* renderer, int32_t surfaceID, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xorig, int32_t yorig) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    DataWin* dw = renderer->dataWin;
    if (0 >= w || 0 >= h) return -1;
    if (!rrSurfaceValid(rr, surfaceID)) return -1;

    uint32_t tpagIndex = rrFindOrAllocTpagSlot(rr);
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    tpag->sourceX = 0;
    tpag->sourceY = 0;
    tpag->sourceWidth = (uint16_t) w;
    tpag->sourceHeight = (uint16_t) h;
    tpag->targetX = 0;
    tpag->targetY = 0;
    tpag->targetWidth = (uint16_t) w;
    tpag->targetHeight = (uint16_t) h;
    tpag->boundingWidth = (uint16_t) w;
    tpag->boundingHeight = (uint16_t) h;
    // Distinct page ID past the real texture pages so the slot counts as "in use"
    tpag->texturePageId = (int16_t) (dw->txtr.count + (tpagIndex - rr->origTpagCount));

    uint32_t slot = tpagIndex - rr->origTpagCount;
    if (slot >= rr->dynTpagSlots) {
        uint32_t newSlots = slot + 1;
        rr->dynDims = (int32_t*) safeRealloc(rr->dynDims, newSlots * 2 * sizeof(int32_t));
        rr->dynTpagSlots = newSlots;
    }
    rr->dynDims[slot * 2] = w;
    rr->dynDims[slot * 2 + 1] = h;

    uint32_t spriteIndex = DataWin_allocSpriteSlot(dw, rr->origSpriteCount);
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    sprite->width = (uint32_t) w;
    sprite->height = (uint32_t) h;
    sprite->originX = xorig;
    sprite->originY = yorig;
    sprite->textureCount = 1;
    sprite->tpagIndices = (int32_t*) safeMalloc(sizeof(int32_t));
    sprite->tpagIndices[0] = (int32_t) tpagIndex;
    sprite->maskCount = 0;
    sprite->masks = nullptr;

    NetWriter* wr = rrBegin(rr, NROP_createSpriteFromSurface, false);
    NetWriter_i32(wr, surfaceID);
    NetWriter_i32(wr, x);
    NetWriter_i32(wr, y);
    NetWriter_i32(wr, w);
    NetWriter_i32(wr, h);
    NetWriter_bool(wr, removeback);
    NetWriter_bool(wr, smooth);
    NetWriter_i32(wr, xorig);
    NetWriter_i32(wr, yorig);
    NetWriter_u32(wr, (uint32_t) tpagIndex);
    NetWriter_u32(wr, spriteIndex);
    return (int32_t) spriteIndex;
}

// ---- Shader handles ----

static int32_t rrDefineHandle(RemoteRenderer* rr, int32_t shaderIndex, const char* name, bool sampler) {
    DataWin* dw = rr->base.dataWin;
    int32_t target = (shaderIndex != -1) ? shaderIndex : rr->base.currentShader;
    if (target < 0 || (uint32_t) target >= dw->shdr.count || name == nullptr || !rr->shadersSupported) return -1;
    for (ptrdiff_t i = 0; i < arrlen(rr->uniforms); i++) {
        RrUniform* u = &rr->uniforms[i];
        if (u->sampler == sampler && u->shader == target && strcmp(u->name, name) == 0) return u->id;
    }
    RrUniform u;
    u.shader = target;
    u.name = safeStrdup(name);
    u.sampler = sampler;
    u.id = rr->nextHandle++;
    arrput(rr->uniforms, u);
    NetWriter* w = rrBegin(rr, sampler ? NROP_shaderGetSamplerIndex : NROP_shaderGetUniform, false);
    NetWriter_i32(w, u.id);
    NetWriter_i32(w, target);
    NetWriter_str(w, name);
    return u.id;
}

static int32_t rrEnc_shaderGetUniform(Renderer* renderer, int32_t shaderIndex, char* uniform) {
    return rrDefineHandle((RemoteRenderer*) renderer, shaderIndex, uniform, false);
}

static int32_t rrEnc_shaderGetSamplerIndex(Renderer* renderer, int32_t shaderIndex, char* uniform) {
    return rrDefineHandle((RemoteRenderer*) renderer, shaderIndex, uniform, true);
}

static void rrEnc_shaderSetUniformFArray(Renderer* renderer, int32_t handle, float* values, uint32_t count) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    if (values == nullptr || count == 0) return;
    NetWriter* w = rrBegin(rr, NROP_shaderSetUniformFArray, false);
    NetWriter_i32(w, handle);
    NetWriter_u32(w, count);
    for (uint32_t i = 0; i < count; i++) NetWriter_f32(w, values[i]);
}

static bool rrEnc_shaderIsCompiled(Renderer* renderer, int32_t shader) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    return rr->shadersSupported && shader >= 0 && (uint32_t) shader < renderer->dataWin->shdr.count;
}

static bool rrEnc_shadersSupported(void) {
    return true;
}

// ---- Texture handles (same encoding as the GL renderers) ----

// Looks up the pixel size of the texture page backing a tpag entry
static bool rrTpagPageSize(RemoteRenderer* rr, int32_t tpagIndex, int32_t* outW, int32_t* outH) {
    DataWin* dw = rr->base.dataWin;
    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return false;
    if ((uint32_t) tpagIndex >= rr->origTpagCount) {
        uint32_t slot = (uint32_t) tpagIndex - rr->origTpagCount;
        if (dw->tpag.items[tpagIndex].texturePageId < 0 || slot >= rr->dynTpagSlots) return false;
        *outW = rr->dynDims[slot * 2];
        *outH = rr->dynDims[slot * 2 + 1];
        return true;
    }
    int16_t page = dw->tpag.items[tpagIndex].texturePageId;
    if (page < 0 || (uint32_t) page >= dw->txtr.count) return false;
    if (rr->pageDims == nullptr) {
        rr->pageCount = dw->txtr.count;
        rr->pageDims = (int32_t*) safeCalloc(rr->pageCount * 2, sizeof(int32_t));
    }
    if (rr->pageDims[page * 2] == 0) {
        Texture* tex = &dw->txtr.textures[page];
        int32_t pw = tex->textureWidth, ph = tex->textureHeight;
        if (pw <= 0 || ph <= 0) {
            // Parse the PNG IHDR (big-endian width / height at offset 16)
            bool freeAfter = (tex->blobData == nullptr);
            DataWin_loadTxtrIfNeeded(dw, (uint32_t) page);
            if (tex->blobData != nullptr && tex->blobSize >= 24) {
                const uint8_t* p = tex->blobData;
                pw = (int32_t) (((uint32_t) p[16] << 24) | ((uint32_t) p[17] << 16) | ((uint32_t) p[18] << 8) | p[19]);
                ph = (int32_t) (((uint32_t) p[20] << 24) | ((uint32_t) p[21] << 16) | ((uint32_t) p[22] << 8) | p[23]);
            }
            if (freeAfter && tex->blobData != nullptr) {
                free(tex->blobData);
                tex->blobData = nullptr;
            }
        }
        if (pw <= 0 || ph <= 0) return false;
        rr->pageDims[page * 2] = pw;
        rr->pageDims[page * 2 + 1] = ph;
    }
    *outW = rr->pageDims[page * 2];
    *outH = rr->pageDims[page * 2 + 1];
    return true;
}

static uint32_t rrEnc_spriteGetTexture(Renderer* renderer, int32_t tpagIndex) {
    int32_t w, h;
    if (!rrTpagPageSize((RemoteRenderer*) renderer, tpagIndex, &w, &h)) return 0;
    return (uint32_t) (tpagIndex + 1);
}

static uint32_t rrEnc_surfaceGetTexture(Renderer* renderer, int32_t surfaceID) {
    if (!rrSurfaceValid((RemoteRenderer*) renderer, surfaceID)) return 0;
    return NET_SURFACE_TEXTURE_FLAG | (uint32_t) surfaceID;
}

// Resolves a handle to its size and (optionally) the tpag sub-rectangle
static bool rrResolveTexture(RemoteRenderer* rr, uint32_t handle, TexturePageItem** outTpag, int32_t* outW, int32_t* outH) {
    if (handle == 0) return false;
    if (handle & NET_SURFACE_TEXTURE_FLAG) {
        uint32_t sid = handle & ~NET_SURFACE_TEXTURE_FLAG;
        if (!rrSurfaceValid(rr, (int32_t) sid)) return false;
        *outTpag = nullptr;
        *outW = rr->surfaces[sid].w;
        *outH = rr->surfaces[sid].h;
        return true;
    }
    int32_t tpagIndex = (int32_t) handle - 1;
    if (!rrTpagPageSize(rr, tpagIndex, outW, outH)) return false;
    *outTpag = &rr->base.dataWin->tpag.items[tpagIndex];
    return true;
}

static float rrEnc_textureGetTexelWidth(Renderer* renderer, uint32_t texID) {
    TexturePageItem* t; int32_t w = 0, h = 0;
    if (!rrResolveTexture((RemoteRenderer*) renderer, texID, &t, &w, &h) || 0 >= w) return 1.0f;
    return 1.0f / (float) w;
}

static float rrEnc_textureGetTexelHeight(Renderer* renderer, uint32_t texID) {
    TexturePageItem* t; int32_t w = 0, h = 0;
    if (!rrResolveTexture((RemoteRenderer*) renderer, texID, &t, &w, &h) || 0 >= h) return 1.0f;
    return 1.0f / (float) h;
}

static bool rrEnc_textureGetUVs(Renderer* renderer, uint32_t texID, float* outUVs) {
    TexturePageItem* tpag; int32_t width = 0, height = 0;
    if (!rrResolveTexture((RemoteRenderer*) renderer, texID, &tpag, &width, &height) || 0 >= width || 0 >= height) return false;
    if (tpag == nullptr) {
        outUVs[0] = 0.0f; outUVs[1] = 0.0f; outUVs[2] = 1.0f; outUVs[3] = 1.0f;
        return true;
    }
    float divW = 1.0f / (float) width;
    float divH = 1.0f / (float) height;
    outUVs[0] = (float) tpag->sourceX * divW;
    outUVs[1] = (float) tpag->sourceY * divH;
    outUVs[2] = outUVs[0] + (float) tpag->sourceWidth * divW;
    outUVs[3] = outUVs[1] + (float) tpag->sourceHeight * divH;
    return true;
}

// ---- GPU state getters ----

static BlendFactors rrEnc_gpuGetBlendFactors(Renderer* renderer) { return ((RemoteRenderer*) renderer)->blendFactors; }
static int32_t rrEnc_gpuGetBlendMode(Renderer* renderer) { return ((RemoteRenderer*) renderer)->blendMode; }
static bool rrEnc_gpuGetAlphaTestEnable(Renderer* renderer) { return ((RemoteRenderer*) renderer)->alphaTestEnable; }
static bool rrEnc_gpuGetBlendEnable(Renderer* renderer) { return ((RemoteRenderer*) renderer)->blendEnable; }
static void rrEnc_gpuGetColorWriteEnable(Renderer* renderer, bool* red, bool* green, bool* blue, bool* alpha) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    if (red) *red = rr->cwR;
    if (green) *green = rr->cwG;
    if (blue) *blue = rr->cwB;
    if (alpha) *alpha = rr->cwA;
}

// ===[ Lifecycle ]===

static void rrInit(Renderer* renderer, DataWin* dataWin) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    renderer->dataWin = dataWin;
    Matrix4f_identity(&renderer->gmlMatrices[MATRIX_WORLD]);
    rr->origTpagCount = dataWin->tpag.count;
    rr->origSpriteCount = dataWin->sprt.count;
    rr->initialTexFilter = renderer->texFilter;
}

static void rrDestroy(Renderer* renderer) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    NetWriter_free(&rr->w);
    free(rr->surfaces);
    free(rr->dynDims);
    free(rr->pageDims);
    for (ptrdiff_t i = 0; i < arrlen(rr->uniforms); i++) free(rr->uniforms[i].name);
    arrfree(rr->uniforms);
    free(rr);
}

static RendererVtable rrVtable;

Renderer* RemoteRenderer_create(void) {
    RemoteRenderer* rr = (RemoteRenderer*) safeCalloc(1, sizeof(RemoteRenderer));
    NetWriter_init(&rr->w);
    RemoteRenderer_fillPlainVtable(&rrVtable);
    rrVtable.init = rrInit;
    rrVtable.destroy = rrDestroy;
    rrVtable.beginFrame = rrEnc_beginFrame;
    rrVtable.beginGUI = rrEnc_beginGUI;
    rrVtable.createSpriteFromSurface = rrEnc_createSpriteFromSurface;
    rrVtable.createSurface = rrEnc_createSurface;
    rrVtable.ensureApplicationSurface = rrEnc_ensureApplicationSurface;
    rrVtable.setRenderTarget = rrEnc_setRenderTarget;
    rrVtable.surfaceSetPixels = rrEnc_surfaceSetPixels;
    rrVtable.surfaceUploadPixels = rrEnc_surfaceUploadPixels;
    // The GL renderers leave these unset, so the runner takes its generic fallbacks (drawSpritePart etc.), which we record
    rrVtable.drawTile = nullptr;
    rrVtable.drawTiledPart = nullptr;
    (void) rrEnc_drawTile;
    rrVtable.drawVertexBuffer = rrEnc_drawVertexBuffer;
    rrVtable.shaderGetUniform = rrEnc_shaderGetUniform;
    rrVtable.shaderGetSamplerIndex = rrEnc_shaderGetSamplerIndex;
    rrVtable.shaderSetUniformFArray = rrEnc_shaderSetUniformFArray;
    rrVtable.gpuGetBlendFactors = rrEnc_gpuGetBlendFactors;
    rrVtable.gpuGetBlendMode = rrEnc_gpuGetBlendMode;
    rrVtable.gpuGetAlphaTestEnable = rrEnc_gpuGetAlphaTestEnable;
    rrVtable.gpuGetColorWriteEnable = rrEnc_gpuGetColorWriteEnable;
    rrVtable.gpuGetBlendEnable = rrEnc_gpuGetBlendEnable;
    rrVtable.surfaceExists = rrEnc_surfaceExists;
    rrVtable.getSurfaceWidth = rrEnc_getSurfaceWidth;
    rrVtable.getSurfaceHeight = rrEnc_getSurfaceHeight;
    rrVtable.surfaceGetPixels = rrEnc_surfaceGetPixels;
    rrVtable.spriteGetTexture = rrEnc_spriteGetTexture;
    rrVtable.surfaceGetTexture = rrEnc_surfaceGetTexture;
    rrVtable.textureGetTexelWidth = rrEnc_textureGetTexelWidth;
    rrVtable.textureGetTexelHeight = rrEnc_textureGetTexelHeight;
    rrVtable.textureGetUVs = rrEnc_textureGetUVs;
    rrVtable.shaderIsCompiled = rrEnc_shaderIsCompiled;
    rrVtable.shadersSupported = rrEnc_shadersSupported;
    rr->base.vtable = &rrVtable;

    rr->base.drawColor = 0xFFFFFF;
    rr->base.drawAlpha = 1.0f;
    rr->base.drawFont = -1;
    rr->base.circlePrecision = 24;
    rr->base.currentShader = -1;
    rr->base.cameraCurrent = 0;
    Matrix4f_identity(&rr->base.gmlMatrices[MATRIX_WORLD]);
    rr->blendEnable = true;
    rr->blendMode = bm_normal;
    rrShadow_gpuSetBlendMode(rr, bm_normal);
    rr->cwR = rr->cwG = rr->cwB = rr->cwA = true;
    rr->shadersSupported = true;
    rr->nextHandle = NET_VIRTUAL_HANDLE_BASE;
    return &rr->base;
}

// ===[ Public API ]===

NetWriter* RemoteRenderer_commands(Renderer* renderer) {
    return &((RemoteRenderer*) renderer)->w;
}

void RemoteRenderer_beginFrame(Renderer* renderer) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    NetWriter_u8(&rr->w, NROP_FRAME_BEGIN);
}

void RemoteRenderer_endFrame(Renderer* renderer) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    NetWriter_u8(&rr->w, NROP_FRAME_END);
}

uint64_t RemoteRenderer_callCount(Renderer* renderer) {
    return ((RemoteRenderer*) renderer)->callCount;
}

void RemoteRenderer_setCapabilities(Renderer* renderer, bool shadersSupported) {
    ((RemoteRenderer*) renderer)->shadersSupported = shadersSupported;
}

void RemoteRenderer_invalidateSyncedState(Renderer* renderer) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    rr->baseValid = false;
    rr->runnerValid = false;
    memset(rr->viewBlob, 0, sizeof(rr->viewBlob));
    memset(rr->camBlob, 0, sizeof(rr->camBlob));
}

int RemoteRenderer_liveSurfaces(Renderer* renderer, int32_t* out, int max) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    int n = 0;
    for (uint32_t i = 0; i < rr->surfaceCount && n < max; i++) {
        if (rr->surfaces[i].exists) out[n++] = (int32_t) i;
    }
    return n;
}

void RemoteRenderer_writeJoinState(Renderer* renderer, NetWriter* out) {
    RemoteRenderer* rr = (RemoteRenderer*) renderer;
    // Everything below is written into `out` (only the joining client receives it) by temporarily swapping the command writer
    NetWriter saved = rr->w;
    rr->w = *out;

    // The full state dump below is delta-coded against the "last sent" caches. Those caches describe what the clients that are
    // ALREADY connected have received, so the dump must not leave them modified: a change that happened since the last frame
    // (e.g. a room change loading new cameras) would otherwise reach only the joiner and never the others.
    // The caches are the contiguous block of fields from baseValid up to (not including) blendEnable.
    size_t syncOff = offsetof(RemoteRenderer, baseValid);
    size_t syncLen = offsetof(RemoteRenderer, blendEnable) - syncOff;
    uint8_t* savedSync = (uint8_t*) safeMalloc(syncLen);
    memcpy(savedSync, (uint8_t*) rr + syncOff, syncLen);

    // Live surfaces (their pixels live on the other clients' GPUs; the game redraws them).
    // The application surface is created by the next ensureApplicationSurface call, which also maps it.
    for (uint32_t i = 0; i < rr->surfaceCount; i++) {
        if (!rr->surfaces[i].exists) continue;
        if ((int32_t) i == rr->base.runner->applicationSurfaceId) continue; // see below
        NetWriter* w = &rr->w;
        NetWriter_u8(w, NROP_createSurface);
        NetWriter_i32(w, (int32_t) i);
        NetWriter_i32(w, rr->surfaces[i].w);
        NetWriter_i32(w, rr->surfaces[i].h);
    }
    // The application surface (also makes the joiner's renderer / runner point at it before the first frame arrives)
    {
        int32_t appId = rr->base.runner->applicationSurfaceId;
        if (rrSurfaceValid(rr, appId)) {
            rrEnc_ensureApplicationSurface(renderer, rr->surfaces[appId].w, rr->surfaces[appId].h);
        }
    }

    // Shader handle definitions
    for (ptrdiff_t i = 0; i < arrlen(rr->uniforms); i++) {
        RrUniform* u = &rr->uniforms[i];
        NetWriter* w = &rr->w;
        NetWriter_u8(w, u->sampler ? NROP_shaderGetSamplerIndex : NROP_shaderGetUniform);
        NetWriter_i32(w, u->id);
        NetWriter_i32(w, u->shader);
        NetWriter_str(w, u->name);
    }

    // Full delta-coded state
    RemoteRenderer_invalidateSyncedState(renderer);
    rrSyncBase(rr);
    rrSyncRunner(rr);

    // GPU state that differs from the defaults every renderer starts with
    if (!rr->blendEnable) rrEnc_gpuSetBlendEnable(renderer, false);
    if (rr->blendMode == bm_complex) rrEnc_gpuSetBlendModeExt(renderer, rr->blendFactors.src, rr->blendFactors.dst, rr->blendFactors.srcAlpha, rr->blendFactors.dstAlpha);
    else if (rr->blendMode != bm_normal) rrEnc_gpuSetBlendMode(renderer, rr->blendMode);
    if (rr->alphaTestEnable) rrEnc_gpuSetAlphaTestEnable(renderer, true);
    if (rr->alphaTestRef != 0) rrEnc_gpuSetAlphaTestRef(renderer, rr->alphaTestRef);
    if (!(rr->cwR && rr->cwG && rr->cwB && rr->cwA)) rrEnc_gpuSetColorWriteEnable(renderer, rr->cwR, rr->cwG, rr->cwB, rr->cwA);
    if (rr->base.texFilter != rr->initialTexFilter) rrEnc_gpuSetTexFilter(renderer, rr->base.texFilter);
    if (rr->base.currentShader >= 0) rrEnc_gpuSetShader(renderer, rr->base.currentShader);

    // Restore the caches: existing clients still get every change relative to what THEY have. The joiner is covered too,
    // because the next frame's delta (relative to the restored caches) is a superset of what the dump already told it.
    memcpy((uint8_t*) rr + syncOff, savedSync, syncLen);
    free(savedSync);

    *out = rr->w;
    rr->w = saved;
}
