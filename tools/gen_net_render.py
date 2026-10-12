#!/usr/bin/env python3
"""Generates src/net/net_render_gen.inc from the RendererVtable declared in src/renderer.h.

For every renderer vtable function that is a plain "record and replay" call this emits
  - an encoder (the server-side RemoteRenderer entry that serializes the call), and
  - a decoder (the client-side function that replays it on a real Renderer).
Functions that need hand-written logic (they return values the server must answer locally, or need
ID translation) are declared MANUAL below and implemented in net_render_server.c / net_render_client.c.

Run:  python3 tools/gen_net_render.py   (from the repository root)
"""
import re, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(ROOT, 'src/renderer.h')).read()
m = re.search(r'typedef struct \{\s*void \(\*init\)\(Renderer\* renderer, DataWin\* dataWin\);.*?\} RendererVtable;', src, re.S)
body = re.sub(r'//.*', '', m.group(0))
fns = []
for ret, name, args in re.findall(r'([\w\s\*]+?)\s*\(\*(\w+)\)\(([^)]*)\);', body):
    ret = ret.replace('typedef struct {', '').strip().split('\n')[-1].strip()
    params = []
    for a in args.split(','):
        a = a.strip()
        if not a or a == 'void':
            continue
        mm = re.match(r'(.*?)(\w+)$', a)
        t, n = mm.group(1).strip().replace(' *', '*'), mm.group(2)
        params.append((t, n))
    if params and params[0][0] == 'Renderer*':
        params = params[1:]
    fns.append((ret, name, params))

# Functions that are not network operations at all
NONOP = {'init', 'destroy', 'shadersSupported'}
# Functions that only read state (the server answers them from its shadow state) – no opcode
GETTERS = {'gpuGetBlendFactors', 'gpuGetBlendMode', 'gpuGetAlphaTestEnable', 'gpuGetColorWriteEnable', 'gpuGetBlendEnable',
           'surfaceExists', 'getSurfaceWidth', 'getSurfaceHeight', 'surfaceGetPixels', 'spriteGetTexture',
           'surfaceGetTexture', 'textureGetTexelWidth', 'textureGetTexelHeight', 'textureGetUVs', 'shaderIsCompiled'}
# Functions with a hand-written encoder AND decoder (opcode allocated here)
MANUAL = {'beginFrame', 'beginGUI', 'createSpriteFromSurface', 'createSurface', 'ensureApplicationSurface', 'setRenderTarget',
          'surfaceSetPixels', 'surfaceUploadPixels', 'drawTile', 'drawVertexBuffer', 'shaderGetUniform',
          'shaderGetSamplerIndex', 'shaderSetUniformFArray'}
# Plain functions whose server-side shadow state must also be updated: calls rrShadow_<name>(rr, args...) after encoding
HOOKS = {'gpuSetBlendMode', 'gpuSetBlendModeExt', 'gpuSetBlendEnable', 'gpuSetAlphaTestEnable', 'gpuSetAlphaTestRef',
         'gpuSetColorWriteEnable', 'gpuSetTexFilter', 'gpuSetShader', 'gpuResetShader', 'setMatrix', 'deleteSprite',
         'surfaceResize', 'surfaceFree'}
# Functions that read GML runner state (views / cameras / app surface flags) inside the renderer
SYNC_RUNNER = {'beginFrame', 'endFrameInit', 'endFrameEnd', 'beginView', 'endView', 'applyProjection', 'beginGUI',
               'setGuiProjection', 'endGUI', 'setRenderTarget', 'ensureApplicationSurface', 'drawSurface',
               'drawSurfaceColor', 'drawSurfaceTiled', 'surfaceCopy', 'surfaceResize', 'clearScreen', 'flush'}
# Client-side validation. The decoder replays commands from a possibly hostile server on renderers that trust their callers
# (they index tables with the arguments), so unsafe values must never reach them.
#   DEC_GUARD: C condition over the decoded arguments; the call is skipped when it is false
#   DEC_PRE / DEC_POST: statements run before the guard / after a call that went through
DEC_GUARD = {
    'setMatrix': 'matrixType >= 0 && matrixType < MATRICES_MAX',
    'gpuSetShader': 'shaderIndex >= 0 && (uint32_t) shaderIndex < d->dw->shdr.count',
    'beginView': 'rdViewOk(d)',
    'endFrameEnd': 'rdAppSurfaceOk(d)',
    'setGuiProjection': 'guiW >= 1 && guiW <= 65536 && guiH >= 1 && guiH <= 65536',
    'surfaceResize': 'width >= 1 && width <= RD_MAX_SURFACE && height >= 1 && height <= RD_MAX_SURFACE',
    'drawVertex': 'd->primVerts < RD_MAX_PRIM_VERTICES',
}
DEC_PRE = {
    'primitiveBegin': 'd->primVerts = 0;',
    'primitiveBeginTexture': 'd->primVerts = 0;',
}
DEC_POST = {
    'drawVertex': 'd->primVerts++;',
}
# Functions that decoder must skip if the real renderer doesn't implement them
# (everything not required by validateRendererVtable); handled uniformly: skip when pointer is null.

KIND_BY_NAME = {
    'tpagIndex': 'TPAG', 'spriteIndex': 'SPRITE',
    'surfaceID': 'SURF', 'surfaceId': 'SURF', 'destSurfaceID': 'SURF', 'srcSurfaceID': 'SURF', 'targetSurfaceId': 'SURF',
    'texture': 'TEX', 'texID': 'TEX',
    'slot': 'SAMPLER',
    'handle': 'UNIFORM',
}

def kind(t, n):
    if n in KIND_BY_NAME and t in ('int32_t', 'uint32_t'):
        return KIND_BY_NAME[n]
    return {'int32_t': 'I32', 'uint32_t': 'U32', 'uint8_t': 'U8', 'bool': 'BOOL', 'float': 'F32',
            'const char*': 'STR', 'char*': 'STR', 'Matrix4f': 'MAT', 'const Matrix4f*': 'MATP'}.get(t)

out = []
w = out.append
w('// GENERATED by tools/gen_net_render.py from src/renderer.h – DO NOT EDIT.')
w('')
ops = [f for f in fns if f[1] not in NONOP and f[1] not in GETTERS]
opsStart = len(out)
w('typedef enum {')
w('    NROP_STATE = 0,   // base renderer state delta')
w('    NROP_RSTATE,      // runner view/camera state delta')
w('    NROP_FRAME_BEGIN, // start of a frame (the client clears the window)')
w('    NROP_FRAME_END,   // end of a frame (carries no payload; the decoder presents)')
for i, (ret, name, params) in enumerate(ops):
    w('    NROP_%s,' % name)
w('    NROP__COUNT')
w('} NetRenderOp;')
w('')
opsEnd = len(out)

def enc_expr(k, n):
    return {
        'I32': 'NetWriter_i32(w, %s);', 'SPRITE': 'NetWriter_i32(w, %s);', 'SURF': 'NetWriter_i32(w, %s);', 'TPAG': 'NetWriter_i32(w, %s);',
        'U32': 'NetWriter_u32(w, %s);', 'TEX': 'NetWriter_u32(w, %s);',
        'SAMPLER': 'NetWriter_i32(w, %s);', 'UNIFORM': 'NetWriter_i32(w, %s);',
        'U8': 'NetWriter_u8(w, %s);', 'BOOL': 'NetWriter_bool(w, %s);', 'F32': 'NetWriter_f32(w, %s);',
        'STR': 'NetWriter_str(w, %s);',
    }[k] % n

# ---- encoders ----
w('#ifdef NET_RENDER_SERVER')
w('// ===[ Encoders (server) ]===')
for ret, name, params in ops:
    if name in MANUAL:
        continue
    decl = ', '.join(['Renderer* renderer'] + ['%s %s' % (t, n) for t, n in params])
    w('static %s rrEnc_%s(%s) {' % (ret, name, decl))
    w('    RemoteRenderer* rr = (RemoteRenderer*) renderer;')
    w('    NetWriter* w = rrBegin(rr, NROP_%s, %s);' % (name, 'true' if name in SYNC_RUNNER else 'false'))
    if not params:
        w('    (void) w;')
    for t, n in params:
        k = kind(t, n)
        if k == 'MAT':
            w('    for (int i = 0; i < 16; i++) NetWriter_f32(w, %s.m[i]);' % n)
        elif k == 'MATP':
            w('    for (int i = 0; i < 16; i++) NetWriter_f32(w, %s->m[i]);' % n)
        elif k is None:
            raise SystemExit('unsupported param type %s %s in %s' % (t, n, name))
        else:
            w('    ' + enc_expr(k, n))
    if name in HOOKS:
        w('    rrShadow_%s(%s);' % (name, ', '.join(['rr'] + [n for t, n in params])))
    w('}')
    w('')

w('static void RemoteRenderer_fillPlainVtable(RendererVtable* vt) {')
for ret, name, params in ops:
    if name not in MANUAL:
        w('    vt->%s = rrEnc_%s;' % (name, name))
w('}')
w('')

# ---- decoders ----
def dec_expr(k, n, t):
    return {
        'I32': 'int32_t %s = NetReader_i32(r);',
        'SURF': 'int32_t %s = rdMapSurface(d, NetReader_i32(r));',
        'TPAG': 'int32_t %s = rdMapTpag(d, NetReader_i32(r));',
        'SPRITE': 'int32_t %s = rdMapSprite(d, NetReader_i32(r));',
        'U32': 'uint32_t %s = NetReader_u32(r);',
        'TEX': 'uint32_t %s = rdMapTexture(d, NetReader_u32(r));',
        'SAMPLER': 'int32_t %s = rdMapSampler(d, NetReader_i32(r));',
        'UNIFORM': 'int32_t %s = rdMapUniform(d, NetReader_i32(r));',
        'U8': 'uint8_t %s = NetReader_u8(r);',
        'BOOL': 'bool %s = NetReader_bool(r);',
        'F32': 'float %s = NetReader_f32(r);',
        'STR': 'char* %s = NetReader_strDup(r);',
    }[k] % n

w('#endif /* NET_RENDER_SERVER */')
w('')
w('#ifdef NET_RENDER_CLIENT')
w('// ===[ Decoders (client) ]===')
for ret, name, params in ops:
    if name in MANUAL:
        continue
    w('static void rrDec_%s(NetRenderDecoder* d, NetReader* r) {' % name)
    w('    Renderer* renderer = d->renderer;')
    args = []
    free = []
    for t, n in params:
        k = kind(t, n)
        if k == 'MAT':
            w('    Matrix4f %s; for (int i = 0; i < 16; i++) %s.m[i] = NetReader_f32(r);' % (n, n))
            args.append(n)
        elif k == 'MATP':
            w('    Matrix4f %s_; for (int i = 0; i < 16; i++) %s_.m[i] = NetReader_f32(r);' % (n, n))
            args.append('&%s_' % n)
        else:
            w('    ' + dec_expr(k, n, t))
            args.append(n)
            if k == 'STR':
                # A null string on the wire must not reach renderers that call strlen() on it
                w('    if (%s == nullptr) %s = (char*) calloc(1, 1);' % (n, n))
                free.append(n)
    w('    if (r->error) {%s return; }' % ''.join(' free(%s);' % n for n in free))
    if name in DEC_PRE:
        w('    ' + DEC_PRE[name])
    cond = 'renderer->vtable->%s != nullptr' % name
    if name in DEC_GUARD:
        cond += ' && ' + DEC_GUARD[name]
    call = 'renderer->vtable->%s(%s);' % (name, ', '.join(['renderer'] + args))
    if name in DEC_POST:
        w('    if (%s) { %s %s }' % (cond, call, DEC_POST[name]))
    else:
        w('    if (%s) %s' % (cond, call))
    for n in free:
        w('    free(%s);' % n)
    w('}')
    w('')

w('static const RrDecodeFn rrDecoders[NROP__COUNT] = {')
w('    [NROP_STATE] = rrDecState,')
w('    [NROP_RSTATE] = rrDecRunnerState,')
w('    [NROP_FRAME_BEGIN] = nullptr,')
w('    [NROP_FRAME_END] = nullptr,')
for ret, name, params in ops:
    w('    [NROP_%s] = rrDec_%s,' % (name, name))
w('};')
w('')

w('#endif /* NET_RENDER_CLIENT */')
w('')
# Names for diagnostics
w('static const char* const rrOpNames[NROP__COUNT] = {')
w('    [NROP_STATE] = "state", [NROP_RSTATE] = "rstate", [NROP_FRAME_BEGIN] = "frameBegin", [NROP_FRAME_END] = "frameEnd",')
for ret, name, params in ops:
    w('    [NROP_%s] = "%s",' % (name, name))
w('};')
w('')
# Forward declarations for manual decoders
manual_decls = ['static void rrDec_%s(NetRenderDecoder* d, NetReader* r);' % n for n in sorted(MANUAL)]

header = ['// GENERATED by tools/gen_net_render.py – manual decoder forward declarations', ''] + manual_decls + ['']
path = os.path.join(ROOT, 'src/net/net_render_gen.inc')
opsHeader = ['// GENERATED by tools/gen_net_render.py – DO NOT EDIT.', '#ifndef _BS_NET_RENDER_OPS_H_', '#define _BS_NET_RENDER_OPS_H_', ''] + out[opsStart:opsEnd] + ['#endif']
open(os.path.join(ROOT, 'src/net/net_render_ops.h'), 'w').write('\n'.join(opsHeader) + '\n')
body_out = out[:opsStart] + ['#include "net_render_ops.h"', ''] + out[opsEnd:]
open(path, 'w').write('\n'.join(body_out) + '\n')
open(os.path.join(ROOT, 'src/net/net_render_gen_decl.inc'), 'w').write('\n'.join(header) + '\n')
print('wrote', path, len(ops), 'ops')
