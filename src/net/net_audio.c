#include "net_audio.h"

#include "net_util.h"
#include "log.h"
#include "utils.h"
#include "stb_ds.h"

#include <stdlib.h>
#include <string.h>

// Instance / stream IDs at or above this value refer to runtime objects instead of SOND indices
#define NA_INSTANCE_ID_BASE 100000

typedef enum {
    NAOP_playSound,
    NAOP_setSoundSpatial,
    NAOP_setListenerPosition,
    NAOP_stopSound,
    NAOP_stopAll,
    NAOP_pauseSound,
    NAOP_resumeSound,
    NAOP_pauseAll,
    NAOP_resumeAll,
    NAOP_suspend,
    NAOP_resume,
    NAOP_setSoundGain,
    NAOP_setSoundPitch,
    NAOP_setTrackPosition,
    NAOP_setMasterGain,
    NAOP_setMasterGainForListener,
    NAOP_setChannelCount,
    NAOP_setGroupGain,
    NAOP_groupLoad,
    NAOP_createStream,
    NAOP_destroyStream,
    NAOP__COUNT
} NetAudioOp;

// ===[ Server tee ]===

typedef struct {
    int32_t id;
    int32_t soundIndex;
    int32_t priority;
    bool loop;
    float gain;
    float pitch;
    bool gainSet, pitchSet;
} TeeInstance;

typedef struct {
    AudioSystem base;
    AudioSystem* inner;
    NetWriter w;
    TeeInstance* instances; // stb_ds
} NetAudioTee;

#define TEE(a) ((NetAudioTee*) (a))

static void teeInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fs) {
    NetAudioTee* t = TEE(audio);
    t->inner->vtable->init(t->inner, dataWin, fs);
    audio->dw = dataWin;
}

static void teeDestroy(AudioSystem* audio) {
    NetAudioTee* t = TEE(audio);
    t->inner->vtable->destroy(t->inner);
    NetWriter_free(&t->w);
    arrfree(t->instances);
    free(t);
}

static void teeUpdate(AudioSystem* audio, float dt) {
    NetAudioTee* t = TEE(audio);
    t->inner->vtable->update(t->inner, dt);
}

static TeeInstance* teeFind(NetAudioTee* t, int32_t id) {
    for (ptrdiff_t i = arrlen(t->instances) - 1; i >= 0; i--) {
        if (t->instances[i].id == id) return &t->instances[i];
    }
    return nullptr;
}

static void teePrune(NetAudioTee* t) {
    for (ptrdiff_t i = arrlen(t->instances) - 1; i >= 0; i--) {
        if (!t->inner->vtable->isPlaying(t->inner, t->instances[i].id)) arrdelswap(t->instances, i);
    }
}

static int32_t teePlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    NetAudioTee* t = TEE(audio);
    int32_t id = t->inner->vtable->playSound(t->inner, soundIndex, priority, loop);
    // Record even failed plays? No: the client would fail identically and a -1 id cannot be addressed afterwards.
    if (id < 0) return id;
    NetWriter_u8(&t->w, NAOP_playSound);
    NetWriter_i32(&t->w, id);
    NetWriter_i32(&t->w, soundIndex);
    NetWriter_i32(&t->w, priority);
    NetWriter_bool(&t->w, loop);
    if (arrlen(t->instances) >= 256) teePrune(t);
    TeeInstance inst;
    memset(&inst, 0, sizeof(inst));
    inst.id = id;
    inst.soundIndex = soundIndex;
    inst.priority = priority;
    inst.loop = loop;
    arrput(t->instances, inst);
    return id;
}

static void teeSetSoundSpatial(AudioSystem* audio, int32_t id, float x, float y, float z, float ref, float max, float factor) {
    NetAudioTee* t = TEE(audio);
    t->inner->vtable->setSoundSpatial(t->inner, id, x, y, z, ref, max, factor);
    NetWriter_u8(&t->w, NAOP_setSoundSpatial);
    NetWriter_i32(&t->w, id);
    NetWriter_f32(&t->w, x); NetWriter_f32(&t->w, y); NetWriter_f32(&t->w, z);
    NetWriter_f32(&t->w, ref); NetWriter_f32(&t->w, max); NetWriter_f32(&t->w, factor);
}

static void teeSetListenerPosition(AudioSystem* audio, float x, float y, float z) {
    NetAudioTee* t = TEE(audio);
    t->inner->vtable->setListenerPosition(t->inner, x, y, z);
    NetWriter_u8(&t->w, NAOP_setListenerPosition);
    NetWriter_f32(&t->w, x); NetWriter_f32(&t->w, y); NetWriter_f32(&t->w, z);
}

#define TEE_SIMPLE_ID(fn, op) \
static void tee_##fn(AudioSystem* audio, int32_t id) { \
    NetAudioTee* t = TEE(audio); \
    t->inner->vtable->fn(t->inner, id); \
    NetWriter_u8(&t->w, op); \
    NetWriter_i32(&t->w, id); \
}
#define TEE_SIMPLE(fn, op) \
static void tee_##fn(AudioSystem* audio) { \
    NetAudioTee* t = TEE(audio); \
    t->inner->vtable->fn(t->inner); \
    NetWriter_u8(&t->w, op); \
}
TEE_SIMPLE_ID(stopSound, NAOP_stopSound)
TEE_SIMPLE_ID(pauseSound, NAOP_pauseSound)
TEE_SIMPLE_ID(resumeSound, NAOP_resumeSound)
TEE_SIMPLE(stopAll, NAOP_stopAll)
TEE_SIMPLE(pauseAll, NAOP_pauseAll)
TEE_SIMPLE(resumeAll, NAOP_resumeAll)
TEE_SIMPLE(suspend, NAOP_suspend)
TEE_SIMPLE(resume, NAOP_resume)

static bool teeIsPlaying(AudioSystem* audio, int32_t id) {
    NetAudioTee* t = TEE(audio);
    return t->inner->vtable->isPlaying(t->inner, id);
}

static void teeSetSoundGain(AudioSystem* audio, int32_t id, float gain, uint32_t timeMs) {
    NetAudioTee* t = TEE(audio);
    t->inner->vtable->setSoundGain(t->inner, id, gain, timeMs);
    NetWriter_u8(&t->w, NAOP_setSoundGain);
    NetWriter_i32(&t->w, id);
    NetWriter_f32(&t->w, gain);
    NetWriter_u32(&t->w, timeMs);
    TeeInstance* inst = teeFind(t, id);
    if (inst != nullptr) { inst->gain = gain; inst->gainSet = true; }
}

static float teeGetSoundGain(AudioSystem* audio, int32_t id) {
    NetAudioTee* t = TEE(audio);
    return t->inner->vtable->getSoundGain(t->inner, id);
}

static void teeSetSoundPitch(AudioSystem* audio, int32_t id, float pitch) {
    NetAudioTee* t = TEE(audio);
    t->inner->vtable->setSoundPitch(t->inner, id, pitch);
    NetWriter_u8(&t->w, NAOP_setSoundPitch);
    NetWriter_i32(&t->w, id);
    NetWriter_f32(&t->w, pitch);
    TeeInstance* inst = teeFind(t, id);
    if (inst != nullptr) { inst->pitch = pitch; inst->pitchSet = true; }
}

static float teeGetSoundPitch(AudioSystem* audio, int32_t id) {
    NetAudioTee* t = TEE(audio);
    return t->inner->vtable->getSoundPitch(t->inner, id);
}

static float teeGetTrackPosition(AudioSystem* audio, int32_t id) {
    NetAudioTee* t = TEE(audio);
    return t->inner->vtable->getTrackPosition(t->inner, id);
}

static void teeSetTrackPosition(AudioSystem* audio, int32_t id, float pos) {
    NetAudioTee* t = TEE(audio);
    t->inner->vtable->setTrackPosition(t->inner, id, pos);
    NetWriter_u8(&t->w, NAOP_setTrackPosition);
    NetWriter_i32(&t->w, id);
    NetWriter_f32(&t->w, pos);
}

static float teeGetSoundLength(AudioSystem* audio, int32_t id) {
    NetAudioTee* t = TEE(audio);
    return t->inner->vtable->getSoundLength(t->inner, id);
}

static void teeSetMasterGain(AudioSystem* audio, float gain) {
    NetAudioTee* t = TEE(audio);
    t->inner->vtable->setMasterGain(t->inner, gain);
    NetWriter_u8(&t->w, NAOP_setMasterGain);
    NetWriter_f32(&t->w, gain);
}

static void teeSetMasterGainForListener(AudioSystem* audio, float gain, int32_t listener) {
    NetAudioTee* t = TEE(audio);
    t->inner->vtable->setMasterGainForListener(t->inner, gain, listener);
    NetWriter_u8(&t->w, NAOP_setMasterGainForListener);
    NetWriter_f32(&t->w, gain);
    NetWriter_i32(&t->w, listener);
}

static void teeSetChannelCount(AudioSystem* audio, int32_t count) {
    NetAudioTee* t = TEE(audio);
    t->inner->vtable->setChannelCount(t->inner, count);
    NetWriter_u8(&t->w, NAOP_setChannelCount);
    NetWriter_i32(&t->w, count);
}

static void teeSetGroupGain(AudioSystem* audio, int32_t group, float gain, uint32_t timeMs) {
    NetAudioTee* t = TEE(audio);
    t->inner->vtable->setGroupGain(t->inner, group, gain, timeMs);
    NetWriter_u8(&t->w, NAOP_setGroupGain);
    NetWriter_i32(&t->w, group);
    NetWriter_f32(&t->w, gain);
    NetWriter_u32(&t->w, timeMs);
}

static void teeGroupLoad(AudioSystem* audio, int32_t group) {
    NetAudioTee* t = TEE(audio);
    t->inner->vtable->groupLoad(t->inner, group);
    NetWriter_u8(&t->w, NAOP_groupLoad);
    NetWriter_i32(&t->w, group);
}

static bool teeGroupIsLoaded(AudioSystem* audio, int32_t group) {
    NetAudioTee* t = TEE(audio);
    return t->inner->vtable->groupIsLoaded(t->inner, group);
}

static int32_t teeCreateStream(AudioSystem* audio, const char* filename) {
    NetAudioTee* t = TEE(audio);
    int32_t id = t->inner->vtable->createStream(t->inner, filename);
    if (id < 0) return id;
    NetWriter_u8(&t->w, NAOP_createStream);
    NetWriter_i32(&t->w, id);
    NetWriter_str(&t->w, filename);
    return id;
}

static bool teeDestroyStream(AudioSystem* audio, int32_t id) {
    NetAudioTee* t = TEE(audio);
    bool ok = t->inner->vtable->destroyStream(t->inner, id);
    NetWriter_u8(&t->w, NAOP_destroyStream);
    NetWriter_i32(&t->w, id);
    return ok;
}

static AudioSystemVtable teeVtable;

AudioSystem* NetAudioTee_create(AudioSystem* inner) {
    NetAudioTee* t = (NetAudioTee*) safeCalloc(1, sizeof(NetAudioTee));
    t->inner = inner;
    NetWriter_init(&t->w);
    teeVtable.init = teeInit;
    teeVtable.destroy = teeDestroy;
    teeVtable.update = teeUpdate;
    teeVtable.playSound = teePlaySound;
    teeVtable.setSoundSpatial = teeSetSoundSpatial;
    teeVtable.setListenerPosition = teeSetListenerPosition;
    teeVtable.stopSound = tee_stopSound;
    teeVtable.stopAll = tee_stopAll;
    teeVtable.isPlaying = teeIsPlaying;
    teeVtable.pauseSound = tee_pauseSound;
    teeVtable.resumeSound = tee_resumeSound;
    teeVtable.pauseAll = tee_pauseAll;
    teeVtable.resumeAll = tee_resumeAll;
    teeVtable.suspend = tee_suspend;
    teeVtable.resume = tee_resume;
    teeVtable.setSoundGain = teeSetSoundGain;
    teeVtable.getSoundGain = teeGetSoundGain;
    teeVtable.setSoundPitch = teeSetSoundPitch;
    teeVtable.getSoundPitch = teeGetSoundPitch;
    teeVtable.getTrackPosition = teeGetTrackPosition;
    teeVtable.setTrackPosition = teeSetTrackPosition;
    teeVtable.getSoundLength = teeGetSoundLength;
    teeVtable.setMasterGain = teeSetMasterGain;
    teeVtable.setMasterGainForListener = teeSetMasterGainForListener;
    teeVtable.setChannelCount = teeSetChannelCount;
    teeVtable.setGroupGain = teeSetGroupGain;
    teeVtable.groupLoad = teeGroupLoad;
    teeVtable.groupIsLoaded = teeGroupIsLoaded;
    teeVtable.createStream = teeCreateStream;
    teeVtable.destroyStream = teeDestroyStream;
    t->base.vtable = &teeVtable;
    t->base.dw = inner->dw;
    return &t->base;
}

NetWriter* NetAudioTee_commands(AudioSystem* tee) {
    return &TEE(tee)->w;
}

void NetAudioTee_writeJoinState(AudioSystem* audio, NetWriter* out) {
    NetAudioTee* t = TEE(audio);
    teePrune(t);
    for (ptrdiff_t i = 0; i < arrlen(t->instances); i++) {
        TeeInstance* inst = &t->instances[i];
        NetWriter_u8(out, NAOP_playSound);
        NetWriter_i32(out, inst->id);
        NetWriter_i32(out, inst->soundIndex);
        NetWriter_i32(out, inst->priority);
        NetWriter_bool(out, inst->loop);
        float pos = t->inner->vtable->getTrackPosition(t->inner, inst->id);
        if (pos > 0.0f) {
            NetWriter_u8(out, NAOP_setTrackPosition);
            NetWriter_i32(out, inst->id);
            NetWriter_f32(out, pos);
        }
        if (inst->gainSet) {
            NetWriter_u8(out, NAOP_setSoundGain);
            NetWriter_i32(out, inst->id);
            NetWriter_f32(out, t->inner->vtable->getSoundGain(t->inner, inst->id));
            NetWriter_u32(out, 0);
        }
        if (inst->pitchSet) {
            NetWriter_u8(out, NAOP_setSoundPitch);
            NetWriter_i32(out, inst->id);
            NetWriter_f32(out, inst->pitch);
        }
    }
}

// ===[ Client decoder ]===

typedef struct { int32_t key; int32_t value; } IdEntry;

struct NetAudioDecoder {
    AudioSystem* audio;
    uint64_t ops, started;
    IdEntry* idMap; // stb_ds hashmap: server id -> local id
};

NetAudioDecoder* NetAudioDecoder_create(AudioSystem* real) {
    NetAudioDecoder* d = (NetAudioDecoder*) safeCalloc(1, sizeof(NetAudioDecoder));
    d->audio = real;
    hmdefault(d->idMap, -1);
    return d;
}

void NetAudioDecoder_destroy(NetAudioDecoder* d) {
    if (d == nullptr) return;
    hmfree(d->idMap);
    free(d);
}

// Stream file names come from the server and are opened by our audio engine, so they must stay harmless:
// relative paths only (no absolute paths, no drive letters / UNC), audio file extensions only.
// ("../" is allowed: DELTARUNE's chapters keep their music in a sibling folder.)
static bool isSafeStreamName(const char* name) {
    size_t n = strlen(name);
    if (n == 0 || n > 400) return false;
    if (name[0] == '/' || name[0] == '\\') return false;
    if (n >= 2 && name[1] == ':') return false;
    const char* dot = strrchr(name, '.');
    if (dot == nullptr) return false;
    static const char* const allowed[] = { ".ogg", ".wav", ".mp3", ".flac", ".mod", ".it", ".xm", ".s3m" };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        const char* a = allowed[i];
        const char* d = dot;
        while (*a && *d && (*a == (char) (*d | 0x20))) { a++; d++; }
        if (*a == '\0' && *d == '\0') return true;
    }
    return false;
}

// Instance / stream ids are translated, sound resource indices pass through untouched
static int32_t mapId(NetAudioDecoder* d, int32_t id) {
    if (id < NA_INSTANCE_ID_BASE) return id;
    ptrdiff_t i = hmgeti(d->idMap, id);
    return i < 0 ? -1 : d->idMap[i].value;
}

bool NetAudioDecoder_run(NetAudioDecoder* d, const uint8_t* data, size_t len) {
    NetReader rd;
    NetReader_init(&rd, data, len);
    NetReader* r = &rd;
    AudioSystemVtable* vt = d->audio->vtable;
    AudioSystem* a = d->audio;
    while (!NetReader_eof(r)) {
        uint8_t op = NetReader_u8(r);
        d->ops++;
        switch (op) {
            case NAOP_playSound: {
                int32_t serverId = NetReader_i32(r);
                int32_t sound = NetReader_i32(r);
                int32_t prio = NetReader_i32(r);
                bool loop = NetReader_bool(r);
                if (r->error) return false;
                int32_t local = vt->playSound(a, mapId(d, sound), prio, loop);
                if (local >= 0) d->started++;
                if (hmlen(d->idMap) > 100000) hmfree(d->idMap); // long sessions: forget old (long finished) sounds instead of growing forever
                hmput(d->idMap, serverId, local);
                break;
            }
            case NAOP_setSoundSpatial: {
                int32_t id = NetReader_i32(r);
                float x = NetReader_f32(r), y = NetReader_f32(r), z = NetReader_f32(r);
                float ref = NetReader_f32(r), max = NetReader_f32(r), factor = NetReader_f32(r);
                if (r->error) return false;
                int32_t l = mapId(d, id);
                if (l >= 0) vt->setSoundSpatial(a, l, x, y, z, ref, max, factor);
                break;
            }
            case NAOP_setListenerPosition: {
                float x = NetReader_f32(r), y = NetReader_f32(r), z = NetReader_f32(r);
                if (r->error) return false;
                vt->setListenerPosition(a, x, y, z);
                break;
            }
            case NAOP_stopSound: case NAOP_pauseSound: case NAOP_resumeSound: {
                int32_t id = NetReader_i32(r);
                if (r->error) return false;
                int32_t l = mapId(d, id);
                if (l < 0) break;
                if (op == NAOP_stopSound) vt->stopSound(a, l);
                else if (op == NAOP_pauseSound) vt->pauseSound(a, l);
                else vt->resumeSound(a, l);
                break;
            }
            case NAOP_stopAll: vt->stopAll(a); break;
            case NAOP_pauseAll: vt->pauseAll(a); break;
            case NAOP_resumeAll: vt->resumeAll(a); break;
            case NAOP_suspend: vt->suspend(a); break;
            case NAOP_resume: vt->resume(a); break;
            case NAOP_setSoundGain: {
                int32_t id = NetReader_i32(r);
                float gain = NetReader_f32(r);
                uint32_t ms = NetReader_u32(r);
                if (r->error) return false;
                int32_t l = mapId(d, id);
                if (l >= 0) vt->setSoundGain(a, l, gain, ms);
                break;
            }
            case NAOP_setSoundPitch: {
                int32_t id = NetReader_i32(r);
                float pitch = NetReader_f32(r);
                if (r->error) return false;
                int32_t l = mapId(d, id);
                if (l >= 0) vt->setSoundPitch(a, l, pitch);
                break;
            }
            case NAOP_setTrackPosition: {
                int32_t id = NetReader_i32(r);
                float pos = NetReader_f32(r);
                if (r->error) return false;
                int32_t l = mapId(d, id);
                if (l >= 0) vt->setTrackPosition(a, l, pos);
                break;
            }
            case NAOP_setMasterGain: {
                float g = NetReader_f32(r);
                if (r->error) return false;
                vt->setMasterGain(a, g);
                break;
            }
            case NAOP_setMasterGainForListener: {
                float g = NetReader_f32(r);
                int32_t l = NetReader_i32(r);
                if (r->error) return false;
                vt->setMasterGainForListener(a, g, l);
                break;
            }
            case NAOP_setChannelCount: {
                int32_t c = NetReader_i32(r);
                if (r->error) return false;
                vt->setChannelCount(a, c);
                break;
            }
            case NAOP_setGroupGain: {
                int32_t g = NetReader_i32(r);
                float gain = NetReader_f32(r);
                uint32_t ms = NetReader_u32(r);
                if (r->error) return false;
                vt->setGroupGain(a, g, gain, ms);
                break;
            }
            case NAOP_groupLoad: {
                int32_t g = NetReader_i32(r);
                if (r->error) return false;
                vt->groupLoad(a, g);
                break;
            }
            case NAOP_createStream: {
                int32_t serverId = NetReader_i32(r);
                char* name = NetReader_strDup(r);
                if (r->error) { free(name); return false; }
                int32_t local = (name != nullptr && isSafeStreamName(name)) ? vt->createStream(a, name) : -1;
                if (name != nullptr && local < 0 && !isSafeStreamName(name)) logWarn("Net: refused audio stream name from the server: '%.100s'\n", name);
                free(name);
                if (hmlen(d->idMap) > 100000) hmfree(d->idMap);
                hmput(d->idMap, serverId, local);
                break;
            }
            case NAOP_destroyStream: {
                int32_t id = NetReader_i32(r);
                if (r->error) return false;
                int32_t l = mapId(d, id);
                if (l >= 0) vt->destroyStream(a, l);
                break;
            }
            default:
                logError("Net: unknown audio opcode %u\n", (unsigned) op);
                return false;
        }
    }
    return true;
}

void NetAudioDecoder_stats(const NetAudioDecoder* d, uint64_t* outOps, uint64_t* outSoundsStarted) {
    *outOps = d->ops;
    *outSoundsStarted = d->started;
}
