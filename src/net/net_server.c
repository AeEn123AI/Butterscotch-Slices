// winsock2.h has to come before windows.h, which other headers pull in
#include "net_link.h"
#include "net_server.h"

#include "net_audio.h"
#include "net_link.h"
#include "net_proto.h"
#include "net_render.h"
#include "net_util.h"

#include "data_win.h"
#include "file_system.h"
#include "gettime.h"
#include "input_recording.h"
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
#include "vm.h"
#if defined(USE_MINIAUDIO)
#include "ma_audio_system.h"
#endif

#include <math.h>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ===[ State ]===

typedef enum {
    CS_HANDSHAKE, // connected, HELLO not yet received
    CS_LOADING,   // told about the game, waiting for READY
    CS_SYNCING,   // READY received; waiting for an existing client to hand over surface pixels (frames are buffered)
    CS_ACTIVE,    // receives frames
    CS_CLOSING,   // told to go away; the KICK message is still being flushed
} ClientState;

typedef struct {
    int type;
    int32_t arg;
} PendingInputEvent;

typedef struct {
    NetLink* link;
    int id;
    ClientState state;
    char name[48];
    int32_t winW, winH;

    // Input collected since the last game step
    bool keyDown[GML_KEY_COUNT];
    bool keyPressedInWindow[GML_KEY_COUNT];
    PendingInputEvent* events; // stb_ds: mouse + char events in arrival order
    bool mouseValid;
    float mouseNx, mouseNy;
    uint64_t lastMouseActivity;
    bool padConnected;
    uint32_t padButtons;
    float padAxes[GP_AXIS_COUNT];
    float padLT, padRT;

    uint32_t lastInputSeq;    // newest INPUT sequence number received
    uint32_t appliedInputSeq; // newest one that has been applied to a game step (echoed in FRAME for latency measurement)
    // Late-join snapshot in progress (CS_SYNCING)
    uint32_t snapshotId;
    int donorId;
    NetWriter syncPayload;     // the SYNC message, generated when the snapshot was requested
    NetWriter* pendingFrames;  // stb_ds array of frame payloads produced while waiting
    uint64_t connectedAt;
    uint64_t stateSince; // when the client entered its current state (timeouts)
    uint64_t framesSent;       // FRAME messages sent since SYNC
    uint64_t framesAcked;      // ... that the client reports as replayed
    uint64_t sentTime[256];    // when each of the last 256 frames was sent (ring)
    uint64_t lastAckTime;
} Client;

typedef struct {
    NetSocket listener;
    Client** clients; // stb_ds
    int nextClientId;
    uint32_t nextSnapshotId;
    int maxClients;
    int maxLagMs;
    bool runWhenEmpty;

    // Current game
    DataWin* dataWin;
    char* dataWinPath;
    NetFileFingerprint fingerprint;
    char* changeWorkingDir; // sent to clients for game_change resolution (null for the first game)
    char* changeFilename;
    int32_t virtualW, virtualH;
    Runner* runner;
    Renderer* renderer;
    AudioSystem* audio;
    uint32_t frameNumber;

    // Statistics
    uint64_t statRawBytes, statFrames;
    uint64_t statLastLog;
    uint64_t statRawAtLastLog, statFramesAtLastLog, statWireAtLastLog;
    uint64_t totalRawBytes, totalFrames;
} Server;

static volatile sig_atomic_t gStopRequested = 0;

static void onStopSignal(int sig) {
    (void) sig;
    gStopRequested = 1;
}

// ===[ Untrusted input helpers ]===

#define MAX_WINDOW_DIM 16384

// Player names end up in logs: keep printable ASCII only (no control / escape characters)
static void sanitizeName(char* out, size_t cap, const char* in) {
    size_t n = 0;
    for (; in != nullptr && *in && n + 1 < cap && n < 32; in++) {
        unsigned char ch = (unsigned char) *in;
        out[n++] = (ch >= 32 && ch < 127) ? (char) ch : '?';
    }
    out[n] = '\0';
    if (n == 0) snprintf(out, cap, "player");
}

static bool isFiniteFloat(float f) {
    return f == f && f > -1e30f && f < 1e30f;
}

static float netClampFloat(float f, float lo, float hi) {
    if (!isFiniteFloat(f)) return 0.0f;
    return f < lo ? lo : (f > hi ? hi : f);
}

// ===[ Clients ]===

static int activeClientCount(Server* s) {
    int n = 0;
    for (ptrdiff_t i = 0; i < arrlen(s->clients); i++) if (s->clients[i]->state == CS_ACTIVE) n++;
    return n;
}

static void sendKick(Client* c, const char* reason) {
    NetWriter w;
    NetWriter_init(&w);
    NetWriter_str(&w, reason);
    NetLink_send(c->link, NM_KICK, w.data, w.len, false);
    NetWriter_free(&w);
    NetLink_pump(c->link);
}

static void freePendingFrames(Client* c);
static void noteFrameSent(Client* c) {
    c->sentTime[c->framesSent & 255] = nowNanos();
    c->framesSent++;
}
static void finishSync(Server* s, Client* c, const uint8_t* snapshot, size_t snapshotLen);

static void removeClient(Server* s, ptrdiff_t index, const char* why) {
    Client* c = s->clients[index];
    logInfo("Net: client #%d (%s, %s) disconnected: %s\n", c->id, c->name, c->link->peer, why);
    for (ptrdiff_t i = 0; i < arrlen(s->clients); i++) {
        Client* j = s->clients[i];
        if (j != c && j->state == CS_SYNCING && j->donorId == c->id) finishSync(s, j, nullptr, 0);
    }
    freePendingFrames(c);
    NetLink_destroy(c->link);
    arrfree(c->events);
    free(c);
    arrdel(s->clients, index);
}

// Virtual window = the window of the oldest active client, so that the game's idea of "the window" matches a real one
static void updateVirtualWindow(Server* s) {
    for (ptrdiff_t i = 0; i < arrlen(s->clients); i++) {
        Client* c = s->clients[i];
        if (c->state == CS_ACTIVE && c->winW > 0 && c->winH > 0) {
            s->virtualW = c->winW;
            s->virtualH = c->winH;
            return;
        }
    }
}

static void writeGameInfo(Server* s, NetWriter* w) {
    NetWriter_str(w, s->dataWin->gen8.displayName != nullptr ? s->dataWin->gen8.displayName : s->dataWin->gen8.name);
    NetWriter_str(w, s->dataWin->gen8.name);
    NetWriter_i32(w, s->dataWin->gen8.gameID);
    NetWriter_u64(w, s->fingerprint.size);
    NetWriter_fixed64(w, s->fingerprint.hash);
    NetWriter_str(w, s->dataWinPath);
    NetWriter_str(w, s->changeWorkingDir);
    NetWriter_str(w, s->changeFilename);
}

static void sendGameInfo(Server* s, Client* c) {
    NetWriter w;
    NetWriter_init(&w);
    writeGameInfo(s, &w);
    NetLink_send(c->link, NM_GAME_INFO, w.data, w.len, false);
    NetWriter_free(&w);
    c->state = CS_LOADING;
    c->stateSince = nowNanos();
}

static void freePendingFrames(Client* c) {
    for (ptrdiff_t i = 0; i < arrlen(c->pendingFrames); i++) NetWriter_free(&c->pendingFrames[i]);
    arrfree(c->pendingFrames);
    c->pendingFrames = nullptr;
    NetWriter_free(&c->syncPayload);
}

// Completes a late join: SYNC, the donor's surface pixels (if any), then everything that was produced meanwhile
static void finishSync(Server* s, Client* c, const uint8_t* snapshot, size_t snapshotLen) {
    NetLink_send(c->link, NM_SYNC, c->syncPayload.data, c->syncPayload.len, true);
    if (snapshot != nullptr) NetLink_send(c->link, NM_SNAPSHOT, snapshot, snapshotLen, true);
    c->framesSent = c->framesAcked = 0;
    c->lastAckTime = nowNanos();
    for (ptrdiff_t i = 0; i < arrlen(c->pendingFrames); i++) {
        NetLink_send(c->link, NM_FRAME, c->pendingFrames[i].data, c->pendingFrames[i].len, true);
        noteFrameSent(c);
    }
    ptrdiff_t buffered = arrlen(c->pendingFrames);
    freePendingFrames(c);
    c->state = CS_ACTIVE;
    NetLink_setLimits(c->link, 64u << 20, 192u << 20); // active clients may relay surface snapshots
    c->stateSince = nowNanos();
    c->connectedAt = nowNanos();
    logInfo("Net: client #%d (%s) is now active (%d active)%s\n", c->id, c->name, activeClientCount(s),
            snapshot != nullptr ? " [surface snapshot applied]" : "");
    (void) buffered;
}

// Sends the persistent state a client needs before it can follow the frame stream
static void buildSyncPayload(Server* s, NetWriter* payload) {
    NetWriter render, audio;
    NetWriter_init(&render);
    NetWriter_init(&audio);
    RemoteRenderer_writeJoinState(s->renderer, &render);
    NetAudioTee_writeJoinState(s->audio, &audio);
    NetWriter_u32(payload, (uint32_t) audio.len);
    NetWriter_bytes(payload, audio.data, audio.len);
    NetWriter_bytes(payload, render.data, render.len);
    NetWriter_free(&render);
    NetWriter_free(&audio);
}

// Called when a client reports READY: gets it going, asking an existing client for the pixels of live surfaces if there are any
static void startSync(Server* s, Client* c) {
    Client* donor = nullptr;
    for (ptrdiff_t i = 0; i < arrlen(s->clients); i++) {
        if (s->clients[i]->state == CS_ACTIVE) { donor = s->clients[i]; break; }
    }
    int32_t surfaces[256];
    int n = RemoteRenderer_liveSurfaces(s->renderer, surfaces, 256);
    NetWriter_init(&c->syncPayload);
    buildSyncPayload(s, &c->syncPayload);
    if (donor == nullptr || n == 0) {
        finishSync(s, c, nullptr, 0);
        return;
    }
    c->snapshotId = ++s->nextSnapshotId;
    c->donorId = donor->id;
    c->state = CS_SYNCING;
    c->stateSince = nowNanos();
    NetWriter req;
    NetWriter_init(&req);
    NetWriter_u32(&req, c->snapshotId);
    NetWriter_u32(&req, (uint32_t) n);
    for (int i = 0; i < n; i++) NetWriter_i32(&req, surfaces[i]);
    NetLink_send(donor->link, NM_SNAPSHOT_REQ, req.data, req.len, false);
    NetWriter_free(&req);
}

// ===[ Message handling ]===

static void handleInput(Server* s, Client* c, NetReader* r) {
    uint8_t flags = NetReader_u8(r);
    c->lastInputSeq = NetReader_u32(r);
    if (flags & NET_INPUT_HAS_WINDOW) {
        uint32_t w = NetReader_u32(r), h = NetReader_u32(r);
        if (w >= 1 && w <= MAX_WINDOW_DIM && h >= 1 && h <= MAX_WINDOW_DIM) {
            c->winW = (int32_t) w;
            c->winH = (int32_t) h;
        }
    }
    if (flags & NET_INPUT_HAS_EVENTS) {
        uint32_t n = NetReader_u32(r);
        for (uint32_t i = 0; i < n && !r->error; i++) {
            uint8_t type = NetReader_u8(r);
            int32_t arg = NetReader_i32(r);
            switch (type) {
                case NIE_KEY_DOWN:
                    if (arg >= 0 && arg < GML_KEY_COUNT) {
                        c->keyDown[arg] = true;
                        c->keyPressedInWindow[arg] = true;
                    }
                    break;
                case NIE_KEY_UP:
                    if (arg >= 0 && arg < GML_KEY_COUNT) c->keyDown[arg] = false;
                    break;
                case NIE_MOUSE_DOWN:
                case NIE_MOUSE_UP:
                case NIE_WHEEL:
                case NIE_CHAR: {
                    if (type == NIE_CHAR && !(arg == 8 || (arg >= 32 && arg <= 0x10FFFF && !(arg >= 0xD800 && arg <= 0xDFFF) && arg != 127))) break;
                    if (type == NIE_WHEEL && arg != 1 && arg != -1) break;
                    PendingInputEvent e = { type, arg };
                    arrput(c->events, e);
                    if (arrlen(c->events) > 4096) arrdel(c->events, 0);
                    if (type != NIE_CHAR) c->lastMouseActivity = nowNanos();
                    break;
                }
                default:
                    break;
            }
        }
    }
    if (flags & NET_INPUT_HAS_MOUSE) {
        c->mouseNx = netClampFloat(NetReader_f32(r), -4.0f, 5.0f);
        c->mouseNy = netClampFloat(NetReader_f32(r), -4.0f, 5.0f);
        c->mouseValid = true;
        c->lastMouseActivity = nowNanos();
    }
    if (flags & NET_INPUT_HAS_PAD) {
        c->padConnected = NetReader_bool(r);
        c->padButtons = NetReader_u32(r) & ((1u << GP_BUTTON_COUNT) - 1);
        for (int i = 0; i < GP_AXIS_COUNT; i++) c->padAxes[i] = netClampFloat(NetReader_f32(r), -1.0f, 1.0f);
        c->padLT = netClampFloat(NetReader_f32(r), 0.0f, 1.0f);
        c->padRT = netClampFloat(NetReader_f32(r), 0.0f, 1.0f);
    }
    (void) s;
}

// Returns false if the client must be dropped
static bool handleMessage(Server* s, Client* c, uint8_t type, const uint8_t* payload, size_t len, char* whyOut, size_t whyCap) {
    NetReader r;
    NetReader_init(&r, payload, len);
    switch (type) {
        case NM_HELLO: {
            uint32_t version = NetReader_u32(&r);
            char* name = NetReader_strDup(&r);
            uint32_t hw = NetReader_u32(&r), hh = NetReader_u32(&r);
            if (r.error) { free(name); snprintf(whyOut, whyCap, "malformed HELLO"); return false; }
            c->winW = (hw >= 1 && hw <= MAX_WINDOW_DIM) ? (int32_t) hw : 0;
            c->winH = (hh >= 1 && hh <= MAX_WINDOW_DIM) ? (int32_t) hh : 0;
            sanitizeName(c->name, sizeof(c->name), name);
            free(name);
            if (version != NET_PROTOCOL_VERSION) {
                sendKick(c, "protocol version mismatch (client and server must run the same Butterscotch build)");
                snprintf(whyOut, whyCap, "protocol version %u", (unsigned) version);
                return false;
            }
            if (arrlen(s->clients) > s->maxClients) {
                sendKick(c, "server is full");
                snprintf(whyOut, whyCap, "server full");
                return false;
            }
            logInfo("Net: client #%d is '%s' (%s), window %dx%d\n", c->id, c->name, c->link->peer, c->winW, c->winH);
            sendGameInfo(s, c);
            return true;
        }
        case NM_READY:
            if (c->state == CS_LOADING) startSync(s, c);
            return true;
        case NM_SNAPSHOT_DATA: {
            // Pixels from a donor: hand them to the client that is waiting for them
            uint32_t id = NetReader_u32(&r);
            for (ptrdiff_t i = 0; i < arrlen(s->clients); i++) {
                Client* j = s->clients[i];
                if (j->state == CS_SYNCING && j->snapshotId == id && j->donorId == c->id) {
                    finishSync(s, j, payload, len);
                    break;
                }
            }
            return true;
        }
        case NM_INPUT:
            if (c->state == CS_ACTIVE) handleInput(s, c, &r);
            return true;
        case NM_ACK: {
            uint64_t acked = NetReader_u32(&r);
            if (!r.error && acked >= c->framesAcked && acked <= c->framesSent) {
                c->framesAcked = acked;
                c->lastAckTime = nowNanos();
            }
            return true;
        }
        case NM_PING: {
            // Echo the timestamp back
            NetLink_send(c->link, NM_PONG, payload, len, false);
            return true;
        }
        case NM_BYE:
            snprintf(whyOut, whyCap, "left");
            return false;
        case NM_ERROR: {
            char* why = NetReader_strDup(&r);
            if (why != nullptr) Net_sanitizeText(why);
            snprintf(whyOut, whyCap, "client error: %.100s", why != nullptr ? why : "?");
            free(why);
            return false;
        }
        default:
            snprintf(whyOut, whyCap, "unknown message type %u", (unsigned) type);
            return false;
    }
}

// Accepts connections and processes everything the clients sent
static void serviceNetwork(Server* s) {
    while (true) {
        NetSocket fd = Net_accept(s->listener);
        if (fd == NET_INVALID_SOCKET) break;
        // Cap half-open connections so a flood of idle sockets can't exhaust the server
        if (arrlen(s->clients) >= s->maxClients * 2 + 8) {
            Net_closeSocket(fd);
            continue;
        }
        Client* c = (Client*) safeCalloc(1, sizeof(Client));
        c->stateSince = nowNanos();
        c->link = NetLink_create(fd);
        NetLink_setLimits(c->link, 4096, 1u << 20); // strangers may only say hello (raised once they are active)
        c->id = ++s->nextClientId;
        c->state = CS_HANDSHAKE;
        snprintf(c->name, sizeof(c->name), "?");
        arrput(s->clients, c);
        logInfo("Net: connection from %s (client #%d)\n", c->link->peer, c->id);
    }

    for (ptrdiff_t i = arrlen(s->clients) - 1; i >= 0; i--) {
        Client* c = s->clients[i];
        bool alive = NetLink_pump(c->link);
        char why[128] = "connection lost";
        if (c->state == CS_CLOSING) {
            // Only waiting for the farewell message to leave the building
            if (!alive || NetLink_pendingBytes(c->link) == 0 || nowNanos() - c->stateSince > 3000000000ull) {
                removeClient(s, i, "kicked");
            }
            continue;
        }
        uint8_t type;
        const uint8_t* payload;
        size_t len;
        while (alive && NetLink_recv(c->link, &type, &payload, &len)) {
            if (!handleMessage(s, c, type, payload, len, why, sizeof(why))) {
                alive = false;
            }
        }
        // Flush replies (PONG, SYNC, ...) immediately instead of at the next wake-up
        if (alive) alive = NetLink_pump(c->link);
        if (c->link->closed) alive = false;
        // Nobody may make the server buffer unbounded output (e.g. by pinging without ever reading the replies)
        if (alive && c->state != CS_ACTIVE && c->state != CS_CLOSING && NetLink_pendingBytes(c->link) > (8u << 20)) {
            snprintf(why, sizeof(why), "not reading its data");
            alive = false;
        }
        // Clients that connect and never say hello / never finish loading are dropped
        if (alive) {
            uint64_t age = nowNanos() - c->stateSince;
            if (c->state == CS_HANDSHAKE && age > 10ull * 1000000000ull) {
                snprintf(why, sizeof(why), "no HELLO within 10 seconds");
                alive = false;
            } else if (c->state == CS_SYNCING && age > 3ull * 1000000000ull) {
                // The donor didn't answer in time: continue without the pixel snapshot
                finishSync(s, c, nullptr, 0);
            } else if (c->state == CS_LOADING && age > 600ull * 1000000000ull) {
                snprintf(why, sizeof(why), "did not finish loading the game within 10 minutes");
                alive = false;
            }
        }
        if (!alive) {
            // Flush whatever (a KICK) is queued
            NetLink_pump(c->link);
            bool wasActive = c->state == CS_ACTIVE;
            removeClient(s, i, why);
            if (wasActive) updateVirtualWindow(s);
        }
    }
}

// ===[ Input merge ]===

static void applyInput(Server* s) {
    Runner* runner = s->runner;
    for (ptrdiff_t i = 0; i < arrlen(s->clients); i++) s->clients[i]->appliedInputSeq = s->clients[i]->lastInputSeq;
    RunnerKeyboardState* kb = runner->keyboard;

    // Keys: a key is down while ANY client holds it. Edges are derived against the previously merged state so two clients
    // pressing the same key don't produce two presses, and a tap that starts and ends within one step is not lost.
    static bool prevMerged[GML_KEY_COUNT];
    for (int k = 0; k < GML_KEY_COUNT; k++) {
        bool down = false, tapped = false;
        for (ptrdiff_t i = 0; i < arrlen(s->clients); i++) {
            Client* c = s->clients[i];
            if (c->state != CS_ACTIVE) continue;
            down |= c->keyDown[k];
            tapped |= c->keyPressedInWindow[k];
            c->keyPressedInWindow[k] = false;
        }
        bool was = prevMerged[k];
        if (!was && (down || tapped)) RunnerKeyboard_onKeyDown(kb, k);
        if ((was && !down) || (!was && !down && tapped)) RunnerKeyboard_onKeyUp(kb, k);
        prevMerged[k] = down;
    }

    // Mouse: buttons / wheel / characters are applied in arrival order; the cursor follows the most recently active client
    Client* mouseOwner = nullptr;
    for (ptrdiff_t i = 0; i < arrlen(s->clients); i++) {
        Client* c = s->clients[i];
        if (c->state != CS_ACTIVE) continue;
        for (ptrdiff_t e = 0; e < arrlen(c->events); e++) {
            PendingInputEvent* ev = &c->events[e];
            switch (ev->type) {
                case NIE_MOUSE_DOWN: RunnerMouse_onButtonDown(runner->mouse, ev->arg); break;
                case NIE_MOUSE_UP: RunnerMouse_onButtonUp(runner->mouse, ev->arg); break;
                case NIE_WHEEL: RunnerMouse_onWheel(runner->mouse, (double) ev->arg); break;
                case NIE_CHAR: RunnerKeyboard_onCharacter(kb, (unsigned int) ev->arg); break;
                default: break;
            }
        }
        arrsetlen(c->events, 0);
        if (c->mouseValid && (mouseOwner == nullptr || c->lastMouseActivity > mouseOwner->lastMouseActivity)) mouseOwner = c;
    }
    if (mouseOwner != nullptr) {
        // Client positions are normalised to the letterboxed game area; convert into the virtual window the runner uses
        double mx = (double) runner->viewportX + (double) mouseOwner->mouseNx * (double) runner->viewportW;
        double my = (double) runner->viewportY + (double) mouseOwner->mouseNy * (double) runner->viewportH;
        Runner_updateMousePosition(runner, s->virtualW, s->virtualH, mx, my);
    }

    // Gamepad: every client's first pad is OR-merged into slot 0
    GamepadSlot* slot = &runner->gamepads->slots[0];
    memcpy(slot->buttonDownPrev, slot->buttonDown, sizeof(slot->buttonDown));
    memset(slot->buttonDown, 0, sizeof(slot->buttonDown));
    memset(slot->buttonPressed, 0, sizeof(slot->buttonPressed));
    memset(slot->buttonReleased, 0, sizeof(slot->buttonReleased));
    memset(slot->buttonValue, 0, sizeof(slot->buttonValue));
    memset(slot->axisValue, 0, sizeof(slot->axisValue));
    bool anyPad = false;
    for (ptrdiff_t i = 0; i < arrlen(s->clients); i++) {
        Client* c = s->clients[i];
        if (c->state != CS_ACTIVE || !c->padConnected) continue;
        anyPad = true;
        for (int b = 0; b < GP_BUTTON_COUNT; b++) {
            if (c->padButtons & (1u << b)) slot->buttonDown[b] = true;
        }
        for (int a = 0; a < GP_AXIS_COUNT; a++) {
            if (fabsf(c->padAxes[a]) > fabsf(slot->axisValue[a])) slot->axisValue[a] = c->padAxes[a];
        }
        if (c->padLT > slot->buttonValue[6]) slot->buttonValue[6] = c->padLT;
        if (c->padRT > slot->buttonValue[7]) slot->buttonValue[7] = c->padRT;
    }
    runner->gamepads->connectedCount = anyPad ? 1 : 0;
    slot->connected = anyPad;
    if (anyPad) {
        slot->jid = 0;
        snprintf(slot->description, sizeof(slot->description), "Network gamepad");
        slot->guid[0] = '\0';
        for (int b = 0; b < GP_BUTTON_COUNT; b++) {
            if (b != 6 && b != 7) slot->buttonValue[b] = slot->buttonDown[b] ? 1.0f : 0.0f;
            bool was = slot->buttonDownPrev[b];
            if (slot->buttonDown[b] && !was) slot->buttonPressed[b] = true;
            if (!slot->buttonDown[b] && was) slot->buttonReleased[b] = true;
        }
    }
}

// ===[ Frame broadcast ]===

static void broadcastFrame(Server* s, bool present, int32_t gameW, int32_t gameH) {
    NetWriter* rcmds = RemoteRenderer_commands(s->renderer);
    NetWriter* acmds = NetAudioTee_commands(s->audio);
    if (present) RemoteRenderer_endFrame(s->renderer);

    int active = activeClientCount(s);
    if (active > 0) {
        // The part every client gets (audio + draw commands); the small header in front is per client (input ack)
        NetWriter body;
        NetWriter_init(&body);
        NetWriter_u32(&body, (uint32_t) acmds->len);
        NetWriter_bytes(&body, acmds->data, acmds->len);
        NetWriter_bytes(&body, rcmds->data, rcmds->len);
        NetWriter payload;
        NetWriter_init(&payload);
        for (ptrdiff_t i = 0; i < arrlen(s->clients); i++) {
            Client* c = s->clients[i];
            if (c->state != CS_ACTIVE && c->state != CS_SYNCING) continue;
            NetWriter_reset(&payload);
            NetWriter_u32(&payload, s->frameNumber);
            NetWriter_i32(&payload, gameW);
            NetWriter_i32(&payload, gameH);
            NetWriter_u8(&payload, (uint8_t) active);
            NetWriter_u32(&payload, c->appliedInputSeq);
            NetWriter_bytes(&payload, body.data, body.len);
            if (c->state == CS_SYNCING) {
                NetWriter copy;
                NetWriter_init(&copy);
                NetWriter_bytes(&copy, payload.data, payload.len);
                arrput(c->pendingFrames, copy);
                continue;
            }
            NetLink_send(c->link, NM_FRAME, payload.data, payload.len, true);
            noteFrameSent(c);
            s->statRawBytes += payload.len;
            s->totalRawBytes += payload.len;
        }
        s->statFrames++;
        s->totalFrames++;
        NetWriter_free(&body);
        NetWriter_free(&payload);
    }
    NetWriter_reset(rcmds);
    NetWriter_reset(acmds);
}

static void logStats(Server* s, uint64_t now) {
    if (now - s->statLastLog < 5000000000ull) return;
    double secs = (double) (now - s->statLastLog) / 1e9;
    uint64_t wire = 0;
    for (ptrdiff_t i = 0; i < arrlen(s->clients); i++) wire += s->clients[i]->link->statWireOut;
    uint64_t wireDelta = wire - s->statWireAtLastLog;
    uint64_t rawDelta = s->statRawBytes - s->statRawAtLastLog;
    int active = activeClientCount(s);
    if (active > 0) {
        logInfo("Net: %.1f fps | %d client(s) | raw %.1f KB/s | on the wire %.1f KB/s total (%.1f KB/s per client)\n",
                (double) (s->statFrames - s->statFramesAtLastLog) / secs, active,
                (double) rawDelta / 1024.0 / secs, (double) wireDelta / 1024.0 / secs,
                (double) wireDelta / 1024.0 / secs / (double) active);
    }
    s->statLastLog = now;
    s->statRawAtLastLog = s->statRawBytes;
    s->statFramesAtLastLog = s->statFrames;
    s->statWireAtLastLog = wire;
}

// ===[ Window callbacks the game can invoke ]===

static Server* gServer = nullptr;

static bool serverGetWindowSize(int32_t* outW, int32_t* outH) {
    *outW = gServer->virtualW;
    *outH = gServer->virtualH;
    return true;
}
static void serverSetWindowSize(int32_t w, int32_t h) {
    if (w > 0 && h > 0) {
        gServer->virtualW = w;
        gServer->virtualH = h;
    }
}
static void serverSetWindowTitle(const char* title) {
    if (gServer == nullptr || title == nullptr) return;
    NetWriter w;
    NetWriter_init(&w);
    NetWriter_str(&w, title);
    for (ptrdiff_t i = 0; i < arrlen(gServer->clients); i++) {
        if (gServer->clients[i]->state == CS_ACTIVE) NetLink_send(gServer->clients[i]->link, NM_TITLE, w.data, w.len, false);
    }
    NetWriter_free(&w);
}
static bool serverGetWindowPosition(int32_t* outX, int32_t* outY) { *outX = 0; *outY = 0; return true; }
static void serverSetWindowPosition(MAYBE_UNUSED int32_t x, MAYBE_UNUSED int32_t y) {}
static bool serverWindowHasFocus(void) { return true; }
static void serverSetCursor(MAYBE_UNUSED int32_t cursor) {}

// ===[ Main ]===

static void sleepServicing(Server* s, uint64_t untilNanos) {
    while (true) {
        uint64_t now = nowNanos();
        if (now >= untilNanos) return;
        uint64_t remaining = untilNanos - now;
        if (remaining > 2000000) {
            NetSocket socks[64];
            int n = 0;
            socks[n++] = s->listener;
            for (ptrdiff_t i = 0; i < arrlen(s->clients) && n < 64; i++) socks[n++] = s->clients[i]->link->fd;
            int waitMs = (int) ((remaining - 1000000) / 1000000);
            // Delayed (simulated) output must still be released on time
            bool simulating = gNetSim.latencyMs > 0 || gNetSim.jitterMs > 0 || gNetSim.bandwidthKbps > 0;
            if (simulating && waitMs > 1) waitMs = 1;
            Net_waitReadable(socks, n, waitMs);
            serviceNetwork(s);
        } else {
            YIELD();
        }
    }
}

static int NetServer_runImpl(CommandLineArgs args, const char* argv0) {
    if (!Net_initSockets()) {
        logError("Net: could not initialise sockets\n");
        return 1;
    }
    gNetSim.latencyMs = args.netSimLatencyMs;
    gNetSim.jitterMs = args.netSimJitterMs;
    gNetSim.bandwidthKbps = args.netSimBandwidthKbps;
    signal(SIGINT, onStopSignal);
    signal(SIGTERM, onStopSignal);

    Server server;
    memset(&server, 0, sizeof(server));
    gServer = &server;
    server.maxClients = args.netMaxClients > 0 ? args.netMaxClients : 16;
    server.runWhenEmpty = args.netRunWhenEmpty;
    server.maxLagMs = args.netMaxLagMs;

    uint16_t port = (uint16_t) args.netPort;
    server.listener = Net_listen(port, args.netBind);
    if (server.listener == NET_INVALID_SOCKET) {
        logError("Net: could not listen on port %u\n", (unsigned) port);
        return 1;
    }
    port = Net_socketPort(server.listener);
    logInfo("Net: server listening on port %u\n", (unsigned) port);
    if (args.netPortFile != nullptr) {
        FILE* f = fopen(args.netPortFile, "w");
        if (f != nullptr) {
            fprintf(f, "%u\n", (unsigned) port);
            fclose(f);
        }
    }

    char* currentDataWinPath = safeStrdup(args.dataWinPath);
    char** currentGameArgs = nullptr;
    repeat(arrlen(args.gameArgs), i) arrput(currentGameArgs, safeStrdup(args.gameArgs[i]));
    arrins(currentGameArgs, 0, safeStrdup(argv0 != nullptr ? argv0 : ""));

    int exitCode = 0;
    bool shuttingDown = false;
    int32_t inputFrameCount = 0;

    while (!shuttingDown) {
        logInfo("Loading %s...\n", currentDataWinPath);
        DataWinParserOptions options = {0};
        options.parseGen8 = true;
        options.parseOptn = true;
        options.parseLang = true;
        options.parseExtn = true;
        options.parseSond = true;
        options.parseAgrp = true;
        options.parseSprt = true;
        options.parseBgnd = true;
        options.parsePath = true;
        options.parseScpt = true;
        options.parseGlob = true;
        options.parseShdr = true;
        options.parseFont = true;
        options.parseTmln = true;
        options.parseObjt = true;
        options.parseRoom = true;
        options.parseTpag = true;
        options.parseCode = true;
        options.parseVari = true;
        options.parseFunc = true;
        options.parseStrg = true;
        // The server never draws: it only needs texture page sizes, which are read on demand
        options.parseTxtr = true;
        options.lazyLoadTextures = true;
#if defined(USE_MINIAUDIO) || defined(USE_OPENAL)
        options.parseAudo = true;
#endif
        options.skipLoadingPreciseMasksForNonPreciseSprites = true;
        options.loadType = args.loadType;
        options.lazyLoadRooms = args.lazyRooms;
        options.lazyLoadAudio = args.lazyAudio;
        options.eagerlyLoadedRooms = args.eagerRooms;
        DataWin* dataWin = DataWin_parse(currentDataWinPath, options);
        Gen8* gen8 = &dataWin->gen8;
        logInfo("Loaded \"%s\" (%d) successfully! [WAD Version %u / GameMaker version %u.%u.%u.%u]\n", gen8->name, gen8->gameID, gen8->wadVersion, dataWin->detectedFormat.major, dataWin->detectedFormat.minor, dataWin->detectedFormat.release, dataWin->detectedFormat.build);

        server.dataWin = dataWin;
        free(server.dataWinPath);
        server.dataWinPath = safeStrdup(currentDataWinPath);
        if (!Net_fingerprintFile(currentDataWinPath, &server.fingerprint)) {
            logError("Net: could not fingerprint %s\n", currentDataWinPath);
            DataWin_free(dataWin);
            exitCode = 1;
            break;
        }

        VMContext* vm = VM_create(dataWin);
        if (args.hasSeed) vm->hasFixedSeed = true;
        vm->alwaysLogUnknownFunctions = args.alwaysLogUnknownFunctions;

        char* dataWinDir = safeStrdup(currentDataWinPath);
        bsGetDirname(dataWinDir);
        const char* savePath = args.saveFolder != nullptr ? args.saveFolder : dataWinDir;
        OverlayFileSystem* overlayFs = OverlayFileSystem_create(dataWinDir, savePath);
        free(dataWinDir);

        int32_t winW, winH;
        loopResolveWindowSize(&args, gen8->defaultWindowWidth, gen8->defaultWindowHeight, &winW, &winH);
        if (server.virtualW <= 0) { server.virtualW = winW; server.virtualH = winH; }

        Renderer* renderer = RemoteRenderer_create();
        AudioSystem* inner;
#if defined(USE_MINIAUDIO)
        gMaAudioNullDevice = true;
        inner = (AudioSystem*) MaAudioSystem_create(dataWin);
#else
        inner = (AudioSystem*) NoopAudioSystem_create();
#endif
        AudioSystem* audio = NetAudioTee_create(inner);
        server.renderer = renderer;
        server.audio = audio;

        Runner* runner = Runner_create(dataWin, vm, renderer, (FileSystem*) overlayFs, audio, args.seed);
        server.runner = runner;
        runner->debugMode = false;
        runner->osType = args.osType;
        runner->setWindowSize = serverSetWindowSize;
        runner->getWindowSize = serverGetWindowSize;
        runner->setWindowTitle = serverSetWindowTitle;
        runner->windowHasFocus = serverWindowHasFocus;
        runner->setCursor = serverSetCursor;
        runner->getWindowPosition = serverGetWindowPosition;
        runner->setWindowPosition = serverSetWindowPosition;
        runner->currentCursor = GML_CR_DEFAULT;
        Runner_setGameArgs(runner, currentGameArgs, (int32_t) arrlen(currentGameArgs));

        if (args.playbackInputsPath != nullptr) {
            globalInputRecording = InputRecording_createPlayer(args.playbackInputsPath, args.recordInputsPath);
        } else if (args.recordInputsPath != nullptr) {
            globalInputRecording = InputRecording_createRecorder(args.recordInputsPath);
        }

        Runner_initFirstRoom(runner);

        // Clients that were playing the previous game load the new one
        for (ptrdiff_t i = 0; i < arrlen(server.clients); i++) {
            if (server.clients[i]->state != CS_HANDSHAKE) sendGameInfo(&server, server.clients[i]);
            server.clients[i]->stateSince = nowNanos(); // loading the game blocked us; don't count it against anyone's timeout
        }

        uint64_t lastFrameTime = nowNanos();
        uint64_t lastFrameStartTime = lastFrameTime;
        server.statLastLog = lastFrameTime;
        bool gameChange = false;

        while (true) {
            if (gStopRequested) { shuttingDown = true; break; }
            if (runner->shouldExit) { shuttingDown = true; break; }
            if (runner->pendingWorkingDirectory != nullptr && runner->pendingLaunchParameters != nullptr) { gameChange = true; break; }
            if (args.exitAtFrame >= 0 && runner->frameCount >= args.exitAtFrame) {
                logInfo("Exiting at frame %d (--exit-at-frame)\n", runner->frameCount);
                shuttingDown = true;
                break;
            }

            serviceNetwork(&server);
            if (activeClientCount(&server) == 0 && !server.runWhenEmpty) {
                // Nobody is watching: freeze the game until someone connects
                sleepServicing(&server, nowNanos() + 5000000ull);
                lastFrameTime = nowNanos();
                lastFrameStartTime = lastFrameTime;
                continue;
            }
            updateVirtualWindow(&server);

            uint64_t frameStartNow = nowNanos();
            runner->deltaTime = (int64_t) (frameStartNow - lastFrameStartTime) / 1000.0;
            lastFrameStartTime = frameStartNow;

            // ---- input ----
            RunnerKeyboard_beginFrame(runner->keyboard);
            RunnerGamepad_beginFrame(runner->gamepads);
            RunnerMouse_beginFrame(runner->mouse);
            applyInput(&server);
            InputRecording_processFrame(globalInputRecording, runner->keyboard, inputFrameCount++);

            // ---- logic ----
            Runner_step(runner);
            float dt = (float) (runner->deltaTime / 1000000.0);
            if (0.0f > dt) dt = 0.0f;
            if (dt > 0.1f) dt = 0.1f;
            runner->audioSystem->vtable->update(runner->audioSystem, dt);

            // ---- drawing (recorded, not executed) ----
            int32_t fbWidth = server.virtualW, fbHeight = server.virtualH;
            RemoteRenderer_beginFrame(renderer);
            if (!runner->appSurfaceEnabled) {
                runner->applicationWidth = fbWidth;
                runner->applicationHeight = fbHeight;
                runner->usingAppSurface = false;
            } else {
                if (runner->applicationWidth <= 0 || runner->applicationHeight <= 0) {
                    runner->applicationWidth = (int32_t) gen8->defaultWindowWidth;
                    runner->applicationHeight = (int32_t) gen8->defaultWindowHeight;
                }
                runner->usingAppSurface = true;
            }
            int32_t gameW = runner->applicationWidth;
            int32_t gameH = runner->applicationHeight;
            runner->widescreenExtraWidth = 0;
            runner->widescreenExtraHeight = 0;
            if (args.widescreenAspect > 0.0f && runner->usingAppSurface && gameW > 0 && gameH > 0) {
                float nativeAspect = (float) gameW / (float) gameH;
                if (args.widescreenAspect > nativeAspect) {
                    int32_t targetW = (int32_t) ((float) gameH * args.widescreenAspect + 0.5f);
                    if (targetW > gameW) { runner->widescreenExtraWidth = targetW - gameW; gameW = targetW; }
                } else if (args.widescreenAspect < nativeAspect) {
                    int32_t targetH = (int32_t) ((float) gameW / args.widescreenAspect + 0.5f);
                    if (targetH > gameH) { runner->widescreenExtraHeight = targetH - gameH; gameH = targetH; }
                }
            }

            if (getenv("BUTTERSCOTCH_NET_DEBUG") != nullptr) {
                static int32_t lastDbg[4];
                int32_t cur[4] = { gameW, gameH, fbWidth, fbHeight };
                if (memcmp(lastDbg, cur, sizeof(cur)) != 0) {
                    logInfo("Net: frame %d: game %dx%d, virtual window %dx%d, views %d, appSurface %d\n", runner->frameCount, gameW, gameH, fbWidth, fbHeight, runner->viewsEnabled, runner->usingAppSurface);
                    memcpy(lastDbg, cur, sizeof(cur));
                }
            }
            Runner_drawPre(runner, fbWidth, fbHeight);
            Runner_beginFrame(runner, gameW, gameH, fbWidth, fbHeight, fbWidth, fbHeight);
            Runner_drawViews(runner, gameW, gameH, false);
            renderer->vtable->endFrameInit(renderer);
            Runner_drawPost(runner, fbWidth, fbHeight);
            renderer->vtable->endFrameEnd(renderer);
            Runner_drawGUI(runner, fbWidth, fbHeight, gameW, gameH);

            // Like the standalone runner, a frame that ends in a room change is not presented
            bool present = (runner->pendingRoom == -1);
            server.frameNumber = (uint32_t) runner->frameCount; // same numbering as the standalone runner's --screenshot-at-frame
            broadcastFrame(&server, present, runner->renderGameW, runner->renderGameH);
            Runner_handlePendingRoomChange(runner);

            // Push the frame onto the wire right away, before sleeping (this is the latency-critical moment)
            for (ptrdiff_t i = 0; i < arrlen(server.clients); i++) NetLink_pump(server.clients[i]->link);

            // Clients that fall behind (slow link or slow machine) are cut off instead of getting ever more lag:
            // the oldest frame still unacknowledged has been in flight for too long
            {
                uint64_t now = nowNanos();
                for (ptrdiff_t i = arrlen(server.clients) - 1; i >= 0; i--) {
                    Client* c = server.clients[i];
                    if (c->state != CS_ACTIVE || c->framesSent == c->framesAcked) continue;
                    uint64_t outstanding = c->framesSent - c->framesAcked;
                    uint64_t lag = outstanding > 240 ? ~0ull : now - c->sentTime[c->framesAcked & 255];
                    bool responsive = now - c->lastAckTime < 1000000000ull;
                    uint64_t limit = (uint64_t) server.maxLagMs * 1000000ull;
                    bool tooSlow = server.maxLagMs > 0 && ((responsive && lag > limit) || lag > limit * 5);
                    if (tooSlow || NetLink_pendingBytes(c->link) > (64u << 20)) {
                        // Discard the backlog, deliver the reason, and let the connection drain in the background
                        NetLink_dropPending(c->link);
                        NetWriter kw;
                        NetWriter_init(&kw);
                        char reason[160];
                        if (responsive) snprintf(reason, sizeof(reason), "your connection (or machine) is too slow for this game's stream: you fell more than %.1f seconds behind", (double) server.maxLagMs / 1000.0);
                        else snprintf(reason, sizeof(reason), "your client stopped responding");
                        NetWriter_str(&kw, reason);
                        NetLink_send(c->link, NM_KICK, kw.data, kw.len, false);
                        NetWriter_free(&kw);
                        logInfo("Net: client #%d (%s) fell behind (%s) – kicking\n", c->id, c->name, responsive ? "too far" : "unresponsive");
                        c->state = CS_CLOSING;
                        c->stateSince = nowNanos();
                        updateVirtualWindow(&server);
                    }
                }
            }
            logStats(&server, nowNanos());

            // ---- pacing ----
            double effectiveGameSpeed = Runner_getEffectiveGameSpeed(runner);
            if (effectiveGameSpeed > 0.0) {
                double speed = args.speedMultiplier > 0.0 ? args.speedMultiplier : 1.0;
                uint64_t targetFrameTime = (uint64_t) (1000000000.0 / (effectiveGameSpeed * speed));
                sleepServicing(&server, lastFrameTime + targetFrameTime);
            }
            lastFrameTime = nowNanos();
        }

        // ---- teardown of this game ----
        if (globalInputRecording != nullptr) {
            if (globalInputRecording->isRecording) InputRecording_save(globalInputRecording);
            InputRecording_free(globalInputRecording);
            globalInputRecording = nullptr;
        }

        char* nextWorkingDirectory = runner->pendingWorkingDirectory;
        char* nextLaunchParameters = runner->pendingLaunchParameters;
        runner->pendingWorkingDirectory = nullptr;
        runner->pendingLaunchParameters = nullptr;

        if (gameChange) {
            // Stop sending frames to clients: they get new GAME_INFO below once we've loaded the next game
            NetWriter empty;
            NetWriter_init(&empty);
            for (ptrdiff_t i = 0; i < arrlen(server.clients); i++) {
                Client* c = server.clients[i];
                if (c->state == CS_ACTIVE) {
                    NetLink_send(c->link, NM_GAME_CHANGE, empty.data, empty.len, false);
                    c->state = CS_LOADING;
                }
            }
            NetWriter_free(&empty);
        }

        runner->audioSystem->vtable->destroy(runner->audioSystem);
        runner->audioSystem = nullptr;
        renderer->vtable->destroy(renderer);
        Runner_free(runner);
        OverlayFileSystem_destroy(overlayFs);
        VM_free(vm);
        DataWin_free(dataWin);
        server.runner = nullptr;
        server.renderer = nullptr;
        server.audio = nullptr;
        server.dataWin = nullptr;

        if (shuttingDown || !gameChange) {
            free(nextWorkingDirectory);
            free(nextLaunchParameters);
            break;
        }

        // Resolve the next game exactly like the standalone runner does
        char** newArguments = extractRunnerArguments(nextLaunchParameters);
        bool macosGameChange = (args.osType == OS_MACOSX);
        char* dataWinFilename = nullptr;
        if (!macosGameChange) {
            size_t length = arrlen(newArguments);
            for (size_t i = 0; i < length; i++) {
                if (strcmp(newArguments[i], "-game") == 0 && i + 1 < length) {
                    dataWinFilename = safeStrdup(newArguments[i + 1]);
                    break;
                }
            }
        } else {
            dataWinFilename = safeStrdup("game.ios");
        }
        char* newPath = dataWinFilename != nullptr ? loopBuildGameChangeTargetPath(currentDataWinPath, nextWorkingDirectory, dataWinFilename) : nullptr;
        if (newPath == nullptr) {
            logError("Runner: game_change did not provide a usable '-game <file>' entry! Shutting down...\n");
            exitCode = 1;
            free(dataWinFilename);
            free(nextWorkingDirectory);
            free(nextLaunchParameters);
            repeat(arrlen(newArguments), i) free(newArguments[i]);
            arrfree(newArguments);
            break;
        }
        free(server.changeWorkingDir);
        free(server.changeFilename);
        server.changeWorkingDir = safeStrdup(nextWorkingDirectory);
        server.changeFilename = safeStrdup(dataWinFilename);
        free(currentDataWinPath);
        currentDataWinPath = newPath;
        while (arrlen(currentGameArgs) > 1) {
            free(currentGameArgs[1]);
            arrdel(currentGameArgs, 1);
        }
        if (macosGameChange) {
            arrput(currentGameArgs, safeStrdup("-game"));
            arrput(currentGameArgs, safeStrdup(currentDataWinPath));
        } else {
            repeat(arrlen(newArguments), i) arrput(currentGameArgs, safeStrdup(newArguments[i]));
        }
        free(dataWinFilename);
        free(nextWorkingDirectory);
        free(nextLaunchParameters);
        repeat(arrlen(newArguments), i) free(newArguments[i]);
        arrfree(newArguments);
    }

    // ---- shutdown ----
    for (ptrdiff_t i = 0; i < arrlen(server.clients); i++) {
        sendKick(server.clients[i], "server closed");
    }
    for (ptrdiff_t i = arrlen(server.clients) - 1; i >= 0; i--) removeClient(&server, i, "server shutting down");
    arrfree(server.clients);
    Net_closeSocket(server.listener);
    if (server.totalFrames > 0) {
        logInfo("Net: sent %llu frames, %.1f KB raw (%.0f bytes/frame)\n", (unsigned long long) server.totalFrames,
                (double) server.totalRawBytes / 1024.0, (double) server.totalRawBytes / (double) server.totalFrames);
    }
    free(currentDataWinPath);
    free(server.dataWinPath);
    free(server.changeWorkingDir);
    free(server.changeFilename);
    repeat(arrlen(currentGameArgs), i) free(currentGameArgs[i]);
    arrfree(currentGameArgs);
    gServer = nullptr;
    logInfo("Bye! :3\n");
    return exitCode;
}

int NetServer_run(CommandLineArgs args, const char* argv0) {
#ifdef _WIN32
    timeBeginPeriod(1); // 1 ms timer resolution, like the standalone runner (needed for accurate frame pacing)
#endif
    int ret = NetServer_runImpl(args, argv0);
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    return ret;
}
