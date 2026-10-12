#ifndef _BS_NET_RENDER_COMMON_H_
#define _BS_NET_RENDER_COMMON_H_

#include "net_buf.h"
#include "runner.h"

// Same values the GL renderers use for texture handles of surfaces (see gl_common.h)
#define NET_SURFACE_TEXTURE_FLAG 0x40000000u
// Server-assigned virtual handles for shader uniforms / samplers (resolved to real ones by the client)
#define NET_VIRTUAL_HANDLE_BASE 0x1000

// Serializes a camera. Matrices are only sent when they are not the ones Runner_updateCameraViewSimple() derives from the parameters
// (e.g. camera_set_view_mat), which is the common case and saves ~160 bytes per camera update.
static inline void NetRender_writeCamera(NetWriter* w, const GMLCamera* c) {
    NetWriter_bool(w, c->allocated);
    if (!c->allocated) return;
    NetWriter_f32(w, c->viewX);
    NetWriter_f32(w, c->viewY);
    NetWriter_i32(w, c->viewWidth);
    NetWriter_i32(w, c->viewHeight);
    NetWriter_u32(w, c->borderX);
    NetWriter_u32(w, c->borderY);
    NetWriter_i32(w, c->speedX);
    NetWriter_i32(w, c->speedY);
    NetWriter_i32(w, c->objectId);
    NetWriter_f32(w, c->viewAngle);
    GMLCamera tmp = *c;
    Runner_updateCameraViewSimple(&tmp);
    bool derived = memcmp(tmp.viewMatrix.m, c->viewMatrix.m, sizeof(c->viewMatrix.m)) == 0 &&
                   memcmp(tmp.projectionMatrix.m, c->projectionMatrix.m, sizeof(c->projectionMatrix.m)) == 0;
    NetWriter_bool(w, derived);
    if (!derived) {
        for (int i = 0; i < 16; i++) NetWriter_f32(w, c->viewMatrix.m[i]);
        for (int i = 0; i < 16; i++) NetWriter_f32(w, c->projectionMatrix.m[i]);
    }
}

static inline void NetRender_readCamera(NetReader* r, GMLCamera* c) {
    c->allocated = NetReader_bool(r);
    if (!c->allocated) return;
    c->viewX = NetReader_f32(r);
    c->viewY = NetReader_f32(r);
    c->viewWidth = NetReader_i32(r);
    c->viewHeight = NetReader_i32(r);
    c->borderX = NetReader_u32(r);
    c->borderY = NetReader_u32(r);
    c->speedX = NetReader_i32(r);
    c->speedY = NetReader_i32(r);
    c->objectId = NetReader_i32(r);
    c->viewAngle = NetReader_f32(r);
    bool derived = NetReader_bool(r);
    if (derived) {
        Runner_updateCameraViewSimple(c);
    } else {
        for (int i = 0; i < 16; i++) c->viewMatrix.m[i] = NetReader_f32(r);
        for (int i = 0; i < 16; i++) c->projectionMatrix.m[i] = NetReader_f32(r);
    }
}

#endif
