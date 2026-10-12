// winsock2.h has to come before windows.h, which other headers pull in
#include "net_link.h"
#include "net_client.h"

#include "game_scan.h"
#include "net_audio.h"
#include "net_link.h"
#include "net_proto.h"
#include "net_render.h"
#include "net_util.h"

#include "data_win.h"
#include "file_system.h"
#include "gettime.h"
#include "log.h"
#include "loop.h"
#include "noop_audio_system.h"
#include "overlay_file_system.h"
#include "runner.h"
#include "runner_gamepad.h"
#include "runner_keyboard.h"
#include "runner_mouse.h"
#include "stb_ds.h"
#include "utils.h"
#if defined(USE_MINIAUDIO)
#include "ma_audio_system.h"
#endif

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    // Connection
    NetLink* link;
    char serverHost[128];
    uint16_t serverPort;

    // Local copy of the game currently being played
    char* dataWinPath;
    char* rootDir;       // directory of the first game we loaded: game_change may only move around inside it
    char* gameTitle;
    DataWin* dataWin;
    Runner* runner;
    Renderer* renderer;
    AudioSystem* audio;
    OverlayFileSystem* overlayFs;
    NetRenderDecoder* rdec;
    NetAudioDecoder* adec;
    bool gameLoaded;     // renderer / audio / decoders exist
    bool active;         // SYNC received: frames are flowing
    bool awaitingGame;   // waiting for the next GAME_INFO

    // Window
    bool platformUp;
    int32_t fbW, fbH;
    int32_t gameW, gameH; // application surface size reported by the server (for letterboxing)
    char baseTitle[200];
    uint64_t lastTitleUpdate;

    // Input state last reported to the server
    float lastMouseNx, lastMouseNy;
    uint64_t lastMouseSend;
    uint32_t lastPadButtons;
    float lastPadAxes[GP_AXIS_COUNT];
    bool lastPadConnected;
    bool sentWindow;
    int32_t sentWinW, sentWinH;

    // Statistics
    uint32_t lastFrameNumber;
    uint64_t framesReceived, framesPresented;
    uint64_t rttNanos, rttMin, rttMax, rttSamples, rttSum;
    uint64_t lastPing;
    uint64_t startTime;
    int serverClients;
    uint64_t statRaw;
    uint32_t framesSinceSync;
    bool ackDirty;
    uint64_t lastAckSent;
    uint64_t lastAudioUpdate;
    bool wantScreenshot;
    int shotCounter;

    // Input -> frame latency: INPUT messages carry a sequence number, FRAME messages echo the newest one the server applied
    uint32_t inputSeq;
    uint64_t inputSentAt[256];
    uint32_t lastAck;
    uint64_t latSum, latMax, latMin, latCount;
} Client;

static char gLastMessage[512];
const char* NetClient_lastMessage(void) { return gLastMessage; }
static void setLastMessage(const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vsnprintf(gLastMessage, sizeof(gLastMessage), fmt, va);
    va_end(va);
}

static volatile sig_atomic_t gStop = 0;
static void onStop(int sig) { (void) sig; gStop = 1; }

// ===[ Game loading ]===

static void unloadGame(Client* c) {
    if (!c->gameLoaded) return;
    {
        uint64_t ops, started;
        NetAudioDecoder_stats(c->adec, &ops, &started);
        if (ops > 0) logInfo("Net: replayed %llu audio commands (%llu sounds started)\n", (unsigned long long) ops, (unsigned long long) started);
    }
    NetRenderDecoder_destroy(c->rdec);
    NetAudioDecoder_destroy(c->adec);
    c->rdec = nullptr;
    c->adec = nullptr;
    if (c->audio != nullptr) {
        c->audio->vtable->destroy(c->audio);
        c->audio = nullptr;
    }
    if (c->renderer != nullptr) {
        c->renderer->vtable->destroy(c->renderer);
        c->renderer = nullptr;
    }
    if (c->runner != nullptr) {
        RunnerKeyboard_free(c->runner->keyboard);
        RunnerMouse_free(c->runner->mouse);
        RunnerGamepad_free(c->runner->gamepads);
        free(c->runner);
        c->runner = nullptr;
    }
    if (c->overlayFs != nullptr) {
        OverlayFileSystem_destroy(c->overlayFs);
        c->overlayFs = nullptr;
    }
    DataWin_free(c->dataWin);
    c->dataWin = nullptr;
    c->gameLoaded = false;
    c->active = false;
}

static void frameBeginHook(void* userData) {
    (void) userData;
    loopClearHostFramebuffer();
}

// Builds everything needed to replay the stream of a game from its local files
static bool loadGame(Client* c, const CommandLineArgs* args, const char* path) {
    logInfo("Loading %s...\n", path);
    DataWinParserOptions options = {0};
    // The client never runs game code, so it only needs the assets that get drawn / played
    options.parseGen8 = true;
    options.parseOptn = true;
    options.parseLang = true;
    options.parseExtn = true;
    options.parseSond = true;
    options.parseAgrp = true;
    options.parseSprt = true;
    options.parseBgnd = true;
    options.parseShdr = true;
    options.parseFont = true;
    options.parseTpag = true;
    options.parseStrg = true;
    options.parseTxtr = true;
    options.parseObjt = true; // cheap, and keeps asset tables consistent with the server
    options.lazyLoadRooms = true;
#if defined(USE_MINIAUDIO) || defined(USE_OPENAL)
    options.parseAudo = true;
#endif
    options.loadType = args->loadType;
    options.lazyLoadTextures = args->lazyTextures;
    options.lazyLoadAudio = args->lazyAudio;
    DataWin* dw = DataWin_parse(path, options);
    if (dw == nullptr) return false;
    c->dataWin = dw;
    free(c->dataWinPath);
    c->dataWinPath = safeStrdup(path);
    if (c->rootDir == nullptr) {
        c->rootDir = safeStrdup(path);
        bsGetDirname(c->rootDir);
    }
    Gen8* gen8 = &dw->gen8;
    logInfo("Loaded \"%s\" (%d) successfully! [WAD Version %u / GameMaker version %u.%u.%u.%u]\n", gen8->name, gen8->gameID, gen8->wadVersion, dw->detectedFormat.major, dw->detectedFormat.minor, dw->detectedFormat.release, dw->detectedFormat.build);

    char* dir = safeStrdup(path);
    bsGetDirname(dir);
    const char* savePath = args->saveFolder != nullptr ? args->saveFolder : dir;
    c->overlayFs = OverlayFileSystem_create(dir, savePath);
    free(dir);

    int32_t winW, winH;
    loopResolveWindowSize(args, gen8->defaultWindowWidth, gen8->defaultWindowHeight, &winW, &winH);

    if (!c->platformUp) {
        char title[256];
        snprintf(title, sizeof(title), "Butterscotch - %s", gen8->displayName);
        gfx = args->renderer;
        if (!platformInit(winW, winH, title, false)) return false;
        if (!loopInitGraphicsContext()) { platformExit(); return false; }
        c->platformUp = true;
    } else {
        platformSetWindowSize(winW, winH);
    }
    snprintf(c->baseTitle, sizeof(c->baseTitle), "%s", gen8->displayName != nullptr ? gen8->displayName : gen8->name);
    platformSetWindowTitle(c->baseTitle);

    c->renderer = loopCreateRenderer(args);
    if (c->renderer == nullptr) return false;

#if defined(USE_MINIAUDIO)
    gMaAudioNullDevice = false;
    c->audio = (AudioSystem*) MaAudioSystem_create(dw);
#else
    c->audio = (AudioSystem*) NoopAudioSystem_create();
#endif

    // A stand-in Runner: holds the input state the platform backend writes to and the few fields the renderers read.
    // No game code ever runs here.
    Runner* runner = (Runner*) safeCalloc(1, sizeof(Runner));
    runner->dataWin = dw;
    runner->renderer = c->renderer;
    runner->fileSystem = (FileSystem*) c->overlayFs;
    runner->audioSystem = c->audio;
    runner->keyboard = RunnerKeyboard_create();
    runner->gamepads = RunnerGamepad_create();
    runner->mouse = RunnerMouse_create();
    runner->appSurfaceEnabled = true;
    runner->appSurfaceAutoDraw = true;
    runner->usingAppSurface = true;
    runner->applicationWidth = (int32_t) gen8->defaultWindowWidth;
    runner->applicationHeight = (int32_t) gen8->defaultWindowHeight;
    runner->applicationSurfaceId = APPLICATION_SURFACE_ID;
    runner->viewportW = 1;
    runner->viewportH = 1;
    for (int i = 0; i < MAX_SURFACES; i++) runner->surfaceStack[i] = -1;
    runner->osType = args->osType;
    c->renderer->runner = runner;
    c->renderer->texFilter = (dw->optn.info & 0x2) != 0;
    c->runner = runner;
    c->renderer->vtable->init(c->renderer, dw);
    c->audio->vtable->init(c->audio, dw, (FileSystem*) c->overlayFs);
    loopPreloadTextures(c->renderer, dw, args);
    platformInitFunctions(runner);

    c->rdec = NetRenderDecoder_create(c->renderer, runner);
    NetRenderDecoder_setFrameBeginHook(c->rdec, frameBeginHook, c);
    c->adec = NetAudioDecoder_create(c->audio);
    c->gameLoaded = true;
    c->sentWindow = false;
    c->lastAudioUpdate = nowNanos();
    return true;
}

// ===[ Game resolution ]===

typedef struct {
    char* displayName;
    char* name;
    int32_t gameId;
    NetFileFingerprint fp;
    char* serverPath;
    char* changeWorkingDir;
    char* changeFilename;
} GameInfo;

static void freeGameInfo(GameInfo* g) {
    free(g->displayName); free(g->name); free(g->serverPath); free(g->changeWorkingDir); free(g->changeFilename);
    memset(g, 0, sizeof(*g));
}

static bool readGameInfo(GameInfo* g, const uint8_t* payload, size_t len) {
    NetReader r;
    NetReader_init(&r, payload, len);
    g->displayName = NetReader_strDup(&r);
    g->name = NetReader_strDup(&r);
    g->gameId = NetReader_i32(&r);
    g->fp.size = NetReader_u64(&r);
    g->fp.hash = NetReader_fixed64(&r);
    g->serverPath = NetReader_strDup(&r);
    g->changeWorkingDir = NetReader_strDup(&r);
    g->changeFilename = NetReader_strDup(&r);
    if (g->displayName != nullptr) Net_sanitizeText(g->displayName);
    if (g->name != nullptr) Net_sanitizeText(g->name);
    return !r.error;
}

static bool fingerprintMatches(const char* path, const NetFileFingerprint* want) {
    NetFileFingerprint fp;
    return Net_fingerprintFile(path, &fp) && fp.size == want->size && fp.hash == want->hash;
}

// Finds the local file that corresponds to what the server is running. Returns a malloc'd path or null (with `why` filled in).
static char* resolveLocalGame(Client* c, const CommandLineArgs* args, const GameInfo* g, char* why, size_t whyCap) {
    // game_change: the server tells us the relative switch it performed
    if (c->dataWinPath != nullptr && g->changeFilename != nullptr) {
        char* candidate = loopBuildGameChangeTargetPath(c->dataWinPath, g->changeWorkingDir != nullptr ? g->changeWorkingDir : "", g->changeFilename);
        // Never let the server steer us outside the game's own folder (it could otherwise probe for / open arbitrary files)
        if (candidate != nullptr && (c->rootDir == nullptr || !Net_pathInside(c->rootDir, candidate))) {
            snprintf(why, whyCap, "the server asked for a game file outside your game folder; refusing");
            free(candidate);
            return nullptr;
        }
        if (candidate != nullptr && fingerprintMatches(candidate, &g->fp)) return candidate;
        if (candidate != nullptr) {
            snprintf(why, whyCap, "'%s' does not match the server's copy of %s (different version or modified files)", candidate, g->displayName ? g->displayName : "the game");
            free(candidate);
        } else {
            snprintf(why, whyCap, "could not locate the next game file");
        }
        return nullptr;
    }

    // Explicit path from the command line / launcher
    if (args->dataWinPath != nullptr && c->dataWinPath == nullptr) {
        if (fingerprintMatches(args->dataWinPath, &g->fp)) return safeStrdup(args->dataWinPath);
        NetFileFingerprint have;
        if (!Net_fingerprintFile(args->dataWinPath, &have)) {
            snprintf(why, whyCap, "cannot read '%s'", args->dataWinPath);
            return nullptr;
        }
        logWarn("Net: '%s' does not match the server's game (%s) – looking for a matching copy...\n", args->dataWinPath, g->displayName ? g->displayName : "?");
    }

    // Auto-detect
    DetectedGame* games = nullptr;
    char* extra = nullptr;
    if (args->dataWinPath != nullptr) {
        extra = safeStrdup(args->dataWinPath);
        bsGetDirname(extra);
    }
    int count = GameScan_detect(&games, extra);
    free(extra);
    int idx = GameScan_findByFingerprint(games, count, g->fp.size, g->fp.hash);
    char* result = idx >= 0 ? safeStrdup(games[idx].path) : nullptr;
    GameScan_free(games, count);
    if (result == nullptr) {
        snprintf(why, whyCap, "no local copy of '%s' found that is identical to the server's (size %llu bytes). Pass the path of your data.win on the command line, or pick it in the launcher.",
                 g->displayName ? g->displayName : "the game", (unsigned long long) g->fp.size);
    } else {
        logInfo("Net: found matching local copy: %s\n", result);
    }
    return result;
}

// ===[ Input ]===

static void computeLetterbox(int32_t gameW, int32_t gameH, int32_t winW, int32_t winH, int32_t* vx, int32_t* vy, int32_t* vw, int32_t* vh) {
    if (gameW <= 0 || gameH <= 0 || winW <= 0 || winH <= 0) {
        *vx = 0; *vy = 0; *vw = winW > 0 ? winW : 1; *vh = winH > 0 ? winH : 1;
        return;
    }
    int32_t sw, sh;
    if ((gameW * winH) / gameH < winW) {
        sw = (gameW * winH) / gameH;
        sh = winH;
    } else {
        sw = winW;
        sh = (gameH * winW) / gameW;
    }
    *vx = (winW - sw) / 2;
    *vy = (winH - sh) / 2;
    *vw = sw > 0 ? sw : 1;
    *vh = sh > 0 ? sh : 1;
}

static void sendInput(Client* c) {
    Runner* runner = c->runner;
    RunnerKeyboardState* kb = runner->keyboard;
    NetWriter body, events;
    NetWriter_init(&body);
    NetWriter_init(&events);
    uint8_t flags = 0;
    uint32_t eventCount = 0;

    for (int k = 0; k < GML_KEY_COUNT; k++) {
        if (kb->keyPressed[k]) {
            NetWriter_u8(&events, NIE_KEY_DOWN); NetWriter_i32(&events, k); eventCount++;
            // A press and release inside one poll: report both so the tap isn't lost
            if (kb->keyReleased[k]) { NetWriter_u8(&events, NIE_KEY_UP); NetWriter_i32(&events, k); eventCount++; }
        } else if (kb->keyReleased[k]) {
            NetWriter_u8(&events, NIE_KEY_UP); NetWriter_i32(&events, k); eventCount++;
        }
    }
    for (int i = 0; i < kb->charQueueLen; i++) {
        NetWriter_u8(&events, NIE_CHAR); NetWriter_i32(&events, (int32_t) kb->charQueue[i]); eventCount++;
    }
    RunnerMouseState* m = runner->mouse;
    for (int b = 0; b < GML_MOUSE_BUTTON_COUNT; b++) {
        if (m->buttonPressed[b]) { NetWriter_u8(&events, NIE_MOUSE_DOWN); NetWriter_i32(&events, b + 1); eventCount++; }
        if (m->buttonReleased[b]) { NetWriter_u8(&events, NIE_MOUSE_UP); NetWriter_i32(&events, b + 1); eventCount++; }
    }
    if (m->wheelUp) { NetWriter_u8(&events, NIE_WHEEL); NetWriter_i32(&events, 1); eventCount++; }
    if (m->wheelDown) { NetWriter_u8(&events, NIE_WHEEL); NetWriter_i32(&events, -1); eventCount++; }

    NetWriter extra;
    NetWriter_init(&extra);

    if (!c->sentWindow || c->sentWinW != c->fbW || c->sentWinH != c->fbH) {
        flags |= NET_INPUT_HAS_WINDOW;
        NetWriter_u32(&extra, (uint32_t) c->fbW);
        NetWriter_u32(&extra, (uint32_t) c->fbH);
        c->sentWindow = true;
        c->sentWinW = c->fbW;
        c->sentWinH = c->fbH;
    }
    if (eventCount > 0) {
        flags |= NET_INPUT_HAS_EVENTS;
        NetWriter_u32(&extra, eventCount);
        NetWriter_bytes(&extra, events.data, events.len);
    }

    // Mouse position, normalised to the letterboxed game area, rate limited
    double mx = 0, my = 0;
    platformGetMousePos(&mx, &my);
    int32_t vx, vy, vw, vh;
    computeLetterbox(c->gameW, c->gameH, c->fbW, c->fbH, &vx, &vy, &vw, &vh);
    float nx = (float) ((mx - vx) / (double) vw);
    float ny = (float) ((my - vy) / (double) vh);
    uint64_t now = nowNanos();
    bool mouseChanged = nx != c->lastMouseNx || ny != c->lastMouseNy;
    if (mouseChanged && now - c->lastMouseSend >= 8000000ull) {
        flags |= NET_INPUT_HAS_MOUSE;
        NetWriter_f32(&extra, nx);
        NetWriter_f32(&extra, ny);
        c->lastMouseNx = nx;
        c->lastMouseNy = ny;
        c->lastMouseSend = now;
    }

    // Gamepad 0
    GamepadSlot* pad = &runner->gamepads->slots[0];
    uint32_t buttons = 0;
    for (int b = 0; b < GP_BUTTON_COUNT; b++) if (pad->buttonDown[b]) buttons |= 1u << b;
    bool padChanged = pad->connected != c->lastPadConnected || buttons != c->lastPadButtons;
    for (int a = 0; a < GP_AXIS_COUNT; a++) if (pad->axisValue[a] != c->lastPadAxes[a]) padChanged = true;
    if (padChanged) {
        flags |= NET_INPUT_HAS_PAD;
        NetWriter_bool(&extra, pad->connected);
        NetWriter_u32(&extra, buttons);
        for (int a = 0; a < GP_AXIS_COUNT; a++) NetWriter_f32(&extra, pad->axisValue[a]);
        NetWriter_f32(&extra, pad->buttonValue[6]);
        NetWriter_f32(&extra, pad->buttonValue[7]);
        c->lastPadConnected = pad->connected;
        c->lastPadButtons = buttons;
        for (int a = 0; a < GP_AXIS_COUNT; a++) c->lastPadAxes[a] = pad->axisValue[a];
    }

    if (flags != 0) {
        NetWriter_u8(&body, flags);
        NetWriter_u32(&body, ++c->inputSeq);
        c->inputSentAt[c->inputSeq & 255] = nowNanos();
        NetWriter_bytes(&body, extra.data, extra.len);
        NetLink_send(c->link, NM_INPUT, body.data, body.len, false);
    }
    NetWriter_free(&body);
    NetWriter_free(&events);
    NetWriter_free(&extra);
}

// ===[ Message handling ]===

static void updateTitle(Client* c, bool force) {
    uint64_t now = nowNanos();
    if (!force && now - c->lastTitleUpdate < 1000000000ull) return;
    c->lastTitleUpdate = now;
    char title[512];
    if (c->rttSamples > 0) {
        snprintf(title, sizeof(title), "%s [%s:%u | ping %.0f ms | %d player%s]", c->baseTitle, c->serverHost, (unsigned) c->serverPort,
                 (double) c->rttNanos / 1e6, c->serverClients, c->serverClients == 1 ? "" : "s");
    } else {
        snprintf(title, sizeof(title), "%s [%s:%u]", c->baseTitle, c->serverHost, (unsigned) c->serverPort);
    }
    platformSetWindowTitle(title);
}

typedef enum { MSG_OK, MSG_DISCONNECT, MSG_FATAL } MsgResult;

#ifdef ENABLE_NET_FUZZ
// Test hook: BUTTERSCOTCH_NET_RECORD=<file> appends every SYNC / FRAME / SNAPSHOT message (type, length, bytes) so the
// decoder fuzzer below has realistic input to mutate.
static FILE* gRecordFile;
static void recordMessage(uint8_t type, const uint8_t* payload, size_t len) {
    if (gRecordFile == nullptr) {
        const char* path = getenv("BUTTERSCOTCH_NET_RECORD");
        if (path == nullptr) return;
        gRecordFile = fopen(path, "ab");
        if (gRecordFile == nullptr) return;
    }
    uint32_t l = (uint32_t) len;
    fwrite(&type, 1, 1, gRecordFile);
    fwrite(&l, 4, 1, gRecordFile);
    fwrite(payload, 1, len, gRecordFile);
    fflush(gRecordFile);
}
#endif

static MsgResult handleMessage(Client* c, const CommandLineArgs* args, uint8_t type, const uint8_t* payload, size_t len,
                               bool* presentFrame, char* why, size_t whyCap) {
    NetReader r;
    NetReader_init(&r, payload, len);
#ifdef ENABLE_NET_FUZZ
    if (type == NM_SYNC || type == NM_FRAME || type == NM_SNAPSHOT) recordMessage(type, payload, len);
#endif
    switch (type) {
        case NM_GAME_INFO: {
            GameInfo g;
            memset(&g, 0, sizeof(g));
            if (!readGameInfo(&g, payload, len)) { snprintf(why, whyCap, "malformed GAME_INFO"); freeGameInfo(&g); return MSG_FATAL; }
            char* path = resolveLocalGame(c, args, &g, why, whyCap);
            if (path == nullptr) {
                NetWriter w; NetWriter_init(&w); NetWriter_str(&w, why);
                NetLink_send(c->link, NM_ERROR, w.data, w.len, false);
                NetWriter_free(&w);
                NetLink_pump(c->link);
                freeGameInfo(&g);
                return MSG_FATAL;
            }
            unloadGame(c);
            bool ok = loadGame(c, args, path);
            free(path);
            freeGameInfo(&g);
            if (!ok) { snprintf(why, whyCap, "could not load the game"); return MSG_FATAL; }
            NetLink_send(c->link, NM_READY, nullptr, 0, false);
            c->awaitingGame = false;
            return MSG_OK;
        }
        case NM_SYNC: {
            if (!c->gameLoaded) return MSG_OK;
            uint32_t audioLen = NetReader_u32(&r);
            const uint8_t* audio = NetReader_bytes(&r, audioLen);
            if (r.error) { snprintf(why, whyCap, "malformed SYNC"); return MSG_FATAL; }
            bool frameEnd = false;
            if (!NetAudioDecoder_run(c->adec, audio, audioLen) ||
                !NetRenderDecoder_run(c->rdec, payload + r.pos, len - r.pos, &frameEnd)) {
                snprintf(why, whyCap, "protocol error in SYNC");
                return MSG_FATAL;
            }
            c->active = true;
            c->framesSinceSync = 0;
            c->startTime = nowNanos();
            return MSG_OK;
        }
        case NM_FRAME: {
            if (!c->gameLoaded || !c->active) return MSG_OK;
            uint32_t frameNumber = NetReader_u32(&r);
            int32_t gameW = NetReader_i32(&r), gameH = NetReader_i32(&r);
            uint8_t clients = NetReader_u8(&r);
            uint32_t ackSeq = NetReader_u32(&r);
            uint32_t audioLen = NetReader_u32(&r);
            const uint8_t* audio = NetReader_bytes(&r, audioLen);
            if (r.error || gameW < 1 || gameW > 65536 || gameH < 1 || gameH > 65536) { snprintf(why, whyCap, "malformed FRAME"); return MSG_FATAL; }
            // Input->frame latency: only meaningful for inputs that carried a key / button event (events set bit 1 when sent)
            if (ackSeq != c->lastAck && ackSeq != 0) {
                uint64_t sentAt = c->inputSentAt[ackSeq & 255];
                if (sentAt != 0 && ackSeq - c->lastAck < 256) {
                    uint64_t lat = nowNanos() - sentAt;
                    if (c->latCount == 0 || lat < c->latMin) c->latMin = lat;
                    if (lat > c->latMax) c->latMax = lat;
                    c->latSum += lat;
                    c->latCount++;
                }
                c->lastAck = ackSeq;
            }
            if ((gameW != c->gameW || gameH != c->gameH) && getenv("BUTTERSCOTCH_NET_DEBUG") != nullptr)
                logInfo("Net: frame %u: game size %dx%d (window %dx%d)\n", (unsigned) frameNumber, gameW, gameH, c->fbW, c->fbH);
            c->lastFrameNumber = frameNumber;
            c->gameW = gameW;
            c->gameH = gameH;
            c->serverClients = clients;
            c->statRaw += len;
            bool frameEnd = false;
            if (!NetAudioDecoder_run(c->adec, audio, audioLen) ||
                !NetRenderDecoder_run(c->rdec, payload + r.pos, len - r.pos, &frameEnd)) {
                snprintf(why, whyCap, "protocol error in FRAME %u", (unsigned) frameNumber);
                return MSG_FATAL;
            }
            c->framesReceived++;
            c->framesSinceSync++;
            c->ackDirty = true;
            if (frameEnd) *presentFrame = true;
#ifdef ENABLE_SCREENSHOTS
            // Screenshots are taken right after the requested frame has been replayed (even if a newer frame is already queued)
            if (frameEnd) {
                FrameSetEntry* shots = args->screenshotFrames;
                int shotFrame = (int) frameNumber;
                if (hmget(shots, shotFrame)) loopCaptureScreenshot(args->screenshotPattern, shotFrame, c->fbW, c->fbH);
            }
#endif
            return MSG_OK;
        }
        case NM_SNAPSHOT_REQ: {
            // We are the donor: read back the requested surfaces as of the end of the frame we just replayed
            if (!c->gameLoaded) return MSG_OK;
            NetWriter out;
            NetWriter_init(&out);
            if (NetRenderDecoder_buildSnapshot(c->rdec, payload, len, &out) && c->link != nullptr) {
                NetLink_send(c->link, NM_SNAPSHOT_DATA, out.data, out.len, true);
            }
            NetWriter_free(&out);
            return MSG_OK;
        }
        case NM_SNAPSHOT: {
            if (!c->gameLoaded) return MSG_OK;
            int n = NetRenderDecoder_applySnapshot(c->rdec, payload, len, getenv("BUTTERSCOTCH_NET_DEBUG") != nullptr);
            logInfo("Net: restored the contents of %d surface%s from another player\n", n, n == 1 ? "" : "s");
            return MSG_OK;
        }
        case NM_PONG: {
            uint64_t sent = NetReader_fixed64(&r);
            if (r.error) return MSG_OK;
            uint64_t rtt = nowNanos() - sent;
            c->rttNanos = rtt;
            if (c->rttSamples == 0 || rtt < c->rttMin) c->rttMin = rtt;
            if (rtt > c->rttMax) c->rttMax = rtt;
            c->rttSum += rtt;
            c->rttSamples++;
            return MSG_OK;
        }
        case NM_GAME_CHANGE:
            c->active = false;
            c->awaitingGame = true;
            logInfo("Net: server is switching games...\n");
            return MSG_OK;
        case NM_TITLE: {
            char* t = NetReader_strDup(&r);
            if (t != nullptr) Net_sanitizeText(t);
            if (t != nullptr && c->platformUp) {
                snprintf(c->baseTitle, sizeof(c->baseTitle), "%s", t);
                updateTitle(c, true);
            }
            free(t);
            return MSG_OK;
        }
        case NM_KICK: {
            char* reason = NetReader_strDup(&r);
            if (reason != nullptr) Net_sanitizeText(reason);
            snprintf(why, whyCap, "%.400s", reason != nullptr ? reason : "disconnected by the server");
            free(reason);
            return MSG_DISCONNECT;
        }
        default:
            snprintf(why, whyCap, "unknown message type %u", (unsigned) type);
            return MSG_FATAL;
    }
}

// ===[ Main ]===

static int NetClient_runImpl(CommandLineArgs args, const char* argv0) {
    (void) argv0;
    gLastMessage[0] = '\0';
    if (!Net_initSockets()) {
        logError("Net: could not initialise sockets\n");
        return 1;
    }
    gNetSim.latencyMs = args.netSimLatencyMs;
    gNetSim.jitterMs = args.netSimJitterMs;
    gNetSim.bandwidthKbps = args.netSimBandwidthKbps;
    signal(SIGINT, onStop);
    signal(SIGTERM, onStop);

    Client client;
    memset(&client, 0, sizeof(client));
    Client* c = &client;
    if (!Net_parseHostPort(args.netConnect, c->serverHost, sizeof(c->serverHost), &c->serverPort, NET_DEFAULT_PORT)) {
        logError("Net: invalid server address '%s' (expected host or host:port)\n", args.netConnect != nullptr ? args.netConnect : "");
        setLastMessage("'%s' is not a valid server address (use host or host:port).", args.netConnect != nullptr ? args.netConnect : "");
        return 1;
    }

    logInfo("Net: connecting to %s:%u...\n", c->serverHost, (unsigned) c->serverPort);
    NetSocket fd = NET_INVALID_SOCKET;
    uint64_t deadline = nowNanos() + 10ull * 1000000000ull;
    while (fd == NET_INVALID_SOCKET) {
        fd = Net_connect(c->serverHost, c->serverPort, 2000);
        if (fd != NET_INVALID_SOCKET) break;
        if (gStop || nowNanos() > deadline) {
            logError("Net: could not connect to %s:%u\n", c->serverHost, (unsigned) c->serverPort);
            setLastMessage("Could not connect to %s:%u (is the server running, and is the port reachable?).", c->serverHost, (unsigned) c->serverPort);
            return 1;
        }
        Net_waitReadable(nullptr, 0, 250);
    }
    c->link = NetLink_create(fd);
    c->awaitingGame = true;

    {
        NetWriter w;
        NetWriter_init(&w);
        NetWriter_u32(&w, NET_PROTOCOL_VERSION);
        NetWriter_str(&w, args.netName != nullptr ? args.netName : "player");
        NetWriter_u32(&w, 0);
        NetWriter_u32(&w, 0);
        NetLink_send(c->link, NM_HELLO, w.data, w.len, false);
        NetWriter_free(&w);
    }

    int exitCode = 0;
    char why[512] = "";
    c->lastPing = 0;
    c->startTime = nowNanos();
    uint64_t lastFrameTimeForAudio = nowNanos();

    while (!gStop) {
        // ---- local input / window ----
        bool presentFrame = false;
        if (c->gameLoaded) {
            RunnerKeyboard_beginFrame(c->runner->keyboard);
            RunnerGamepad_beginFrame(c->runner->gamepads);
            RunnerMouse_beginFrame(c->runner->mouse);
            if (platformHandleEvents()) break;
            if (RunnerKeyboard_checkPressed(c->runner->keyboard, VK_F5)) c->wantScreenshot = true; // local screenshot hotkey
            {
                int32_t oldW = c->fbW, oldH = c->fbH;
                platformGetWindowSize(&c->fbW, &c->fbH);
                if ((c->fbW != oldW || c->fbH != oldH) && getenv("BUTTERSCOTCH_NET_DEBUG") != nullptr)
                    logInfo("Net: window size is now %dx%d (frame %u)\n", c->fbW, c->fbH, (unsigned) c->lastFrameNumber);
            }
            if (c->rdec != nullptr) NetRenderDecoder_setWindowSize(c->rdec, c->fbW, c->fbH);
            if (c->active) sendInput(c);
        }

        // ---- network ----
        bool alive = NetLink_pump(c->link);
        uint8_t type;
        const uint8_t* payload;
        size_t len;
        bool fatal = false;
        while (alive && NetLink_recv(c->link, &type, &payload, &len)) {
            MsgResult res = handleMessage(c, &args, type, payload, len, &presentFrame, why, sizeof(why));
            if (res == MSG_DISCONNECT) {
                logInfo("Net: %s\n", why);
                if (strcmp(why, "server closed") != 0) setLastMessage("Disconnected by the server: %s", why);
                alive = false;
                exitCode = (strcmp(why, "server closed") == 0) ? 0 : 1;
                fatal = true;
                break;
            }
            if (res == MSG_FATAL) {
                logError("Net: %s\n", why);
                setLastMessage("%s", why);
                exitCode = 1;
                fatal = true;
                alive = false;
                break;
            }
        }
        if (fatal) break;
        if (!alive || c->link->closed) {
            logError("Net: connection to the server was lost\n");
            setLastMessage("The connection to the server was lost.");
            exitCode = 1;
            break;
        }

        // ---- present ----
        if (presentFrame && c->gameLoaded) {
            c->framesPresented++;
#ifdef ENABLE_SCREENSHOTS
            if (c->wantScreenshot) {
                c->wantScreenshot = false;
                loopCaptureScreenshot(args.screenshotPattern != nullptr ? args.screenshotPattern : "screenshot-%d.png", c->shotCounter++, c->fbW, c->fbH);
            }
#endif
            platformSwapBuffers();
            updateTitle(c, false);
            if (args.exitAtFrame >= 0 && (int) c->lastFrameNumber >= args.exitAtFrame) {
                logInfo("Exiting at frame %u (--exit-at-frame)\n", (unsigned) c->lastFrameNumber);
                break;
            }
        }

        // ---- audio housekeeping (fades, ended sounds) on local time ----
        if (c->gameLoaded && c->audio != nullptr) {
            uint64_t now = nowNanos();
            float dt = (float) ((double) (now - lastFrameTimeForAudio) / 1e9);
            if (dt >= 0.01f) {
                if (dt > 0.1f) dt = 0.1f;
                c->audio->vtable->update(c->audio, dt);
                lastFrameTimeForAudio = now;
            }
        }

        // ---- progress report for the server's lag watchdog (every ~60 ms while frames flow) ----
        if (c->ackDirty && nowNanos() - c->lastAckSent >= 60000000ull) {
            NetWriter w;
            NetWriter_init(&w);
            NetWriter_u32(&w, c->framesSinceSync);
            NetLink_send(c->link, NM_ACK, w.data, w.len, false);
            NetWriter_free(&w);
            c->ackDirty = false;
            c->lastAckSent = nowNanos();
        }

        // ---- latency probe ----
        uint64_t now = nowNanos();
        if (now - c->lastPing >= 500000000ull && !c->awaitingGame) {
            NetWriter w;
            NetWriter_init(&w);
            NetWriter_fixed64(&w, now);
            NetLink_send(c->link, NM_PING, w.data, w.len, false);
            NetWriter_free(&w);
            c->lastPing = now;
        }

        // ---- wait for more data without adding latency ----
        if (!presentFrame) {
            NetSocket s = c->link->fd;
            bool simulating = gNetSim.latencyMs > 0 || gNetSim.jitterMs > 0 || gNetSim.bandwidthKbps > 0;
            Net_waitReadable(&s, 1, (simulating || !c->gameLoaded) ? 1 : 2);
        }
    }

    if (c->link != nullptr) {
        NetLink_send(c->link, NM_BYE, nullptr, 0, false);
        NetLink_pump(c->link);
    }
    if (c->framesReceived > 0 || c->rttSamples > 0) {
        double secs = (double) (nowNanos() - c->startTime) / 1e9;
        logInfo("Net: received %llu frames (%llu presented) in %.1fs, %.1f KB on the wire (%.1f KB/s), raw %.1f KB\n",
                (unsigned long long) c->framesReceived, (unsigned long long) c->framesPresented, secs,
                (double) c->link->statWireIn / 1024.0, (double) c->link->statWireIn / 1024.0 / (secs > 0 ? secs : 1),
                (double) c->statRaw / 1024.0);
        if (c->latCount > 0) {
            logInfo("Net: input -> next frame latency min %.1f / avg %.1f / max %.1f ms (%llu inputs)\n",
                    (double) c->latMin / 1e6, (double) c->latSum / (double) c->latCount / 1e6, (double) c->latMax / 1e6,
                    (unsigned long long) c->latCount);
        }
        if (c->rttSamples > 0) {
            logInfo("Net: round-trip time min %.1f / avg %.1f / max %.1f ms (%llu samples)\n",
                    (double) c->rttMin / 1e6, (double) c->rttSum / (double) c->rttSamples / 1e6, (double) c->rttMax / 1e6,
                    (unsigned long long) c->rttSamples);
        }
    }
    NetLink_destroy(c->link);
    unloadGame(c);
    free(c->dataWinPath);
    free(c->rootDir);
    if (c->platformUp) platformExit();
    return exitCode;
}


#ifdef ENABLE_NET_FUZZ
// ===[ Decoder fuzzer ]===
// Replays mutated copies of recorded server messages on the real renderer / audio decoders, in one process, so that
// AddressSanitizer / UBSan see everything a hostile server could make the client do.

static uint64_t fuzzState;
static uint32_t fuzzRand(void) {
    fuzzState = fuzzState * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t) (fuzzState >> 33);
}

typedef struct { uint8_t type; uint8_t* data; size_t len; } CorpusMsg;

static void mutate(uint8_t* buf, size_t* len, size_t cap) {
    static const uint8_t interesting[] = { 0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE, 0x10, 0x20, 0x0F };
    int rounds = 1 + (int) (fuzzRand() % 4);
    for (int i = 0; i < rounds && *len > 0; i++) {
        size_t pos = fuzzRand() % *len;
        switch (fuzzRand() % 9) {
            case 0: buf[pos] ^= (uint8_t) (1u << (fuzzRand() % 8)); break;
            case 1: buf[pos] = (uint8_t) fuzzRand(); break;
            case 2: buf[pos] = interesting[fuzzRand() % sizeof(interesting)]; break;
            case 3: { // varint boundary values
                static const uint8_t big[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0x0F };
                for (size_t k = 0; k < sizeof(big) && pos + k < *len; k++) buf[pos + k] = big[k];
                break;
            }
            case 4: { // delete a range
                size_t n = 1 + fuzzRand() % 8;
                if (pos + n > *len) n = *len - pos;
                memmove(buf + pos, buf + pos + n, *len - pos - n);
                *len -= n;
                break;
            }
            case 5: { // duplicate a range
                size_t n = 1 + fuzzRand() % 16;
                if (pos + n > *len) n = *len - pos;
                if (*len + n <= cap) {
                    memmove(buf + pos + n, buf + pos, *len - pos);
                    *len += n;
                }
                break;
            }
            case 6: *len = pos; break; // truncate
            case 7: buf[pos] = (uint8_t) (fuzzRand() % 90); break; // small values are valid opcodes / small numbers
            default: { // insert a random byte
                if (*len + 1 <= cap) {
                    memmove(buf + pos + 1, buf + pos, *len - pos);
                    buf[pos] = (uint8_t) fuzzRand();
                    (*len)++;
                }
            }
        }
    }
}


// Hand-written hostile command streams aimed at the values the renderers would trust (indices, sizes, counts).
// Each one must be rejected or neutralised by the decoder without crashing (run under ASan / UBSan).
#include "net_render_ops.h"
#define A_OP(op) NetWriter_u8(&w, (uint8_t) (op))
#define A_I(v) NetWriter_i32(&w, (int32_t) (v))
#define A_U(v) NetWriter_u32(&w, (uint32_t) (v))
#define A_F(v) NetWriter_f32(&w, (float) (v))
#define A_B(v) NetWriter_bool(&w, (v))
#define A_S(v) NetWriter_str(&w, (v))
#define A_RUN() do { bool fe_; NetRenderDecoder_run(c->rdec, w.data, w.len, &fe_); NetWriter_reset(&w); attacksRun++; } while (0)

static int runAttacks(Client* c) {
    NetWriter w;
    NetWriter_init(&w);
    int attacksRun = 0;
    static const int32_t evil[] = { -1, 0, 1, 7, 8, 100, 4096, 65535, 65536, 1 << 20, 1 << 30, 0x7FFFFFFF, (int32_t) 0x80000000, -2, -1000000 };
    const int E = (int) (sizeof(evil) / sizeof(evil[0]));
    c->runner->applicationSurfaceId = 999999; // a stale application surface id

    for (int i = 0; i < E; i++) {
        int32_t v = evil[i];
        // matrices / shaders / sprites / textures indexed by the server
        A_OP(NROP_setMatrix); A_I(v); for (int k = 0; k < 16; k++) A_F(k); A_RUN();
        A_OP(NROP_gpuSetShader); A_I(v); A_RUN();
        A_OP(NROP_gpuSetShader); A_I(v); A_OP(NROP_shaderSetUniformF); A_I(0x1000 + (v & 0xFF)); A_I(v); A_F(1); A_F(2); A_F(3); A_F(4); A_RUN();
        A_OP(NROP_deleteSprite); A_I(v); A_RUN();
        A_OP(NROP_textureSetStage); A_I(v); A_U(v); A_RUN();
        A_OP(NROP_primitiveBeginTexture); A_I(v); A_I(v); A_OP(NROP_drawVertex); A_F(1); A_F(2); A_F(3); A_U(v); A_F(1); A_F(0); A_F(0); A_OP(NROP_primitiveEnd); A_RUN();
        A_OP(NROP_drawSprite); A_I(v); for (int k = 0; k < 7; k++) A_F(k); A_U(v); A_F(1); A_RUN();
        A_OP(NROP_drawSurface); A_I(v); A_I(v); A_I(v); A_I(v); A_I(v); for (int k = 0; k < 5; k++) A_F(k); A_U(v); A_F(1); A_RUN();
        A_OP(NROP_surfaceCopy); A_I(v); A_I(v); A_I(v); A_I(v); A_I(v); A_I(v); A_I(v); A_I(v); A_B(true); A_RUN();
        // sizes
        A_OP(NROP_createSurface); A_I(i + 5); A_I(v); A_I(v); A_RUN();
        A_OP(NROP_createSurface); A_I(v); A_I(64); A_I(64); A_RUN();
        A_OP(NROP_ensureApplicationSurface); A_I(v); A_I(v); A_I(v); A_RUN();
        A_OP(NROP_surfaceResize); A_I(0); A_I(v); A_I(v); A_RUN();
        A_OP(NROP_createSpriteFromSurface); A_I(0); A_I(v); A_I(v); A_I(v); A_I(v); A_B(false); A_B(false); A_I(0); A_I(0); A_U(v); A_U(v); A_RUN();
        A_OP(NROP_surfaceUploadPixels); A_I(0); A_I(v); A_I(v); for (int k = 0; k < 64; k++) A_I(k); A_RUN();
        A_OP(NROP_surfaceSetPixels); A_I(0); A_U(v); for (int k = 0; k < 64; k++) A_I(k); A_RUN();
        A_OP(NROP_beginFrame); A_I(v); A_I(v); A_I(v); A_I(v); A_RUN();
        A_OP(NROP_beginGUI); A_I(v); A_I(v); A_I(v); A_I(v); A_I(v); A_I(v); A_I(v); A_RUN();
        A_OP(NROP_setGuiProjection); A_I(v); A_I(v); A_I(v); A_I(v); A_B(true); A_RUN();
        A_OP(NROP_endFrameEnd); A_RUN();
        A_OP(NROP_endFrameInit); A_RUN();
        // views / cameras / base state
        A_OP(NROP_RSTATE); A_U(1); A_B(true); A_I(v); A_B(true); A_B(true);
        A_OP(NROP_beginView); A_I(v); A_I(v); A_I(v); A_I(v); A_I(v); A_I(v); A_I(v); A_I(v); A_F(0); A_OP(NROP_endView); A_RUN();
        A_OP(NROP_RSTATE); A_U(2); A_U(1); A_U(0); A_B(true); A_I(0); A_I(0); A_I(100); A_I(100); A_I(v); A_I(v);
        A_OP(NROP_beginView); A_I(0); A_I(0); A_I(10); A_I(10); A_I(0); A_I(0); A_I(10); A_I(10); A_F(0); A_OP(NROP_endView); A_RUN();
        A_OP(NROP_setRenderTarget); A_I(v); A_B(true); A_RUN();
        A_OP(NROP_STATE); A_U(127); A_U(v); A_F(1); A_I(v); A_I(v); A_I(v); A_I(v); A_I(v);
        A_OP(NROP_drawText); A_S("hello \xff\xfe\xc3 world"); for (int k = 0; k < 6; k++) A_F(k); A_RUN();
        // vertex buffers
        A_OP(NROP_drawVertexBuffer); A_I(v); A_I(v); A_I(v); A_U(v); A_U(2); A_I(1); A_I(0); A_U(v); A_U(v); A_I(v); A_I(v); A_U(v); A_U(v); for (int k = 0; k < 8; k++) A_I(k); A_RUN();
        A_OP(NROP_drawVertexBuffer); A_I(4); A_I(0); A_I(3); A_U(16); A_U(1); A_I(1); A_I(0); A_U(0); A_U(16); for (int k = 0; k < 12; k++) A_F(k); A_RUN();
        // shader handles
        A_OP(NROP_shaderGetUniform); A_I(0x1000 + i); A_I(v); A_S("uTexture"); A_RUN();
        A_OP(NROP_shaderGetSamplerIndex); A_I(v); A_I(v); A_S("gm_BaseTexture"); A_RUN();
        A_OP(NROP_shaderSetUniformFArray); A_I(0x1000); A_U(v); A_F(1); A_RUN();
    }
    // An oversized primitive: far more vertices than any renderer buffer holds
    A_OP(NROP_primitiveBegin); A_I(4); A_RUN();
    for (int i = 0; i < 120000; i++) {
        A_OP(NROP_drawVertex); A_F(i); A_F(1); A_F(0); A_U(0xFFFFFF); A_F(1); A_F(0); A_F(0);
        if ((i & 4095) == 4095) A_RUN();
    }
    A_OP(NROP_primitiveEnd); A_RUN();
    A_OP(NROP_flush); A_RUN();
    // A million-byte string
    {
        char* big = (char*) malloc(1 << 20);
        memset(big, 'A', (1 << 20) - 1);
        big[(1 << 20) - 1] = 0;
        A_OP(NROP_drawText); A_S(big); for (int k = 0; k < 6; k++) A_F(k); A_RUN();
        free(big);
    }
    // Snapshot / title / kick messages with hostile contents
    {
        uint8_t buf[256];
        char why[256];
        bool present;
        for (int i = 0; i < 200; i++) {
            for (size_t k = 0; k < sizeof(buf); k++) buf[k] = (uint8_t) (fuzzRand() >> 3);
            if (i & 1) { buf[0] = 1; buf[1] = 255; buf[2] = 255; buf[3] = 255; buf[4] = 15; }
            handleMessage(c, nullptr, NM_SNAPSHOT, buf, sizeof(buf), &present, why, sizeof(why));
            handleMessage(c, nullptr, NM_SNAPSHOT_REQ, buf, sizeof(buf), &present, why, sizeof(why));
        }
    }
    NetWriter_free(&w);
    return attacksRun;
}

int NetClient_fuzz(CommandLineArgs args, const char* corpusPath, int iterations, unsigned seed) {
    FILE* f = fopen(corpusPath, "rb");
    if (f == nullptr) { logError("fuzz: cannot open corpus '%s'\n", corpusPath); return 1; }
    CorpusMsg* corpus = nullptr;
    uint8_t type;
    uint32_t l;
    while (fread(&type, 1, 1, f) == 1 && fread(&l, 4, 1, f) == 1 && l < (64u << 20)) {
        CorpusMsg m;
        m.type = type;
        m.len = l;
        m.data = (uint8_t*) malloc(l ? l : 1);
        if (fread(m.data, 1, l, f) != l) { free(m.data); break; }
        arrput(corpus, m);
    }
    fclose(f);
    if (arrlen(corpus) == 0) { logError("fuzz: empty corpus\n"); return 1; }
    fprintf(stderr, "fuzz: %d corpus messages, %d iterations, seed %u\n", (int) arrlen(corpus), iterations, seed);

    Client client;
    memset(&client, 0, sizeof(client));
    Client* c = &client;
    snprintf(c->serverHost, sizeof(c->serverHost), "fuzz");
    if (!loadGame(c, &args, args.dataWinPath)) { logError("fuzz: could not load the game\n"); return 1; }
    c->active = true;
    c->fbW = 640;
    c->fbH = 480;
    NetRenderDecoder_setWindowSize(c->rdec, 640, 480);
    fuzzState = ((uint64_t) seed << 32) ^ 0x9E3779B97F4A7C15ull;

    // Establish a realistic state (surfaces, views, ...) by replaying the first part of the recorded stream untouched
    {
        char why0[512];
        bool pres;
        for (int i = 0; i < 60 && i < (int) arrlen(corpus); i++) {
            handleMessage(c, &args, corpus[i].type, corpus[i].data, corpus[i].len, &pres, why0, sizeof(why0));
        }
    }
    fprintf(stderr, "fuzz: hostile command streams...\n");
    int attacks = runAttacks(c);
    fprintf(stderr, "fuzz: %d hostile streams survived\n", attacks);

    static uint8_t buf[1 << 20];
    char why[512];
    int ok = 0, rejected = 0;
    uint64_t t0 = nowNanos();
    for (int it = 0; it < iterations; it++) {
        // Mostly replay the stream in order (keeps decoder state realistic), with mutated messages mixed in
        CorpusMsg* m = &corpus[(it < arrlen(corpus) && fuzzRand() % 4 != 0) ? it : (int) (fuzzRand() % (uint32_t) arrlen(corpus))];
        size_t len = m->len < sizeof(buf) ? m->len : sizeof(buf);
        memcpy(buf, m->data, len);
        uint8_t t = m->type;
        if (fuzzRand() % 10 == 0) t = (uint8_t) (16 + fuzzRand() % 12); // wrong / unexpected message types
        if (fuzzRand() % 8 != 0) mutate(buf, &len, sizeof(buf));
        if (t == NM_GAME_INFO) t = NM_PONG; // would reload the whole game
        c->active = true;
        bool present = false;
        MsgResult res = handleMessage(c, &args, t, buf, len, &present, why, sizeof(why));
        if (res == MSG_OK) ok++; else rejected++;
        if (present) platformSwapBuffers();
        if ((it & 1023) == 1023) fprintf(stderr, "fuzz: %d iterations (%d accepted, %d rejected), %.1fs\n", it + 1, ok, rejected, (double) (nowNanos() - t0) / 1e9);
    }
    fprintf(stderr, "fuzz: done, %d accepted / %d rejected, no crash\n", ok, rejected);
    unloadGame(c);
    if (c->platformUp) platformExit();
    return 0;
}
#endif

int NetClient_run(CommandLineArgs args, const char* argv0) {
#ifdef _WIN32
    timeBeginPeriod(1); // 1 ms timer resolution, like the standalone runner (the client polls input every ~2 ms)
#endif
    int ret = NetClient_runImpl(args, argv0);
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    return ret;
}
