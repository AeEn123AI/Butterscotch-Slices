#ifndef _BS_NET_PROTO_H_
#define _BS_NET_PROTO_H_

// Butterscotch client/server wire protocol.
//
// Transport: one TCP connection per client (TCP_NODELAY). Every message is
//     varint(length) | u8 type | payload          (type bit 7 = payload belongs to the compressed stream)
// Server -> client FRAME / SYNC payloads go through a persistent deflate stream (Z_SYNC_FLUSH per message) so the
// compressor sees the previous frames as history – consecutive frames are nearly identical, so this does most of the work.
//
// The server runs the game; clients only render and send input:
// Late join: surfaces live on the GPUs of the clients, so a joining client gets the persistent state from the server (SYNC)
// and the pixels of existing surfaces from an already connected client, relayed by the server:
//     server -> donor:   SNAPSHOT_REQ(id, surface ids)     (placed in the donor's stream right after the frame the SYNC corresponds to)
//     donor  -> server:  SNAPSHOT_DATA(id, pixels)
//     server -> joiner:  SYNC, SNAPSHOT, then every frame produced while waiting
//
//   client                                   server
//     HELLO(version, name, window) ------->
//                              <------- GAME_INFO(game identity, relative path, virtual window)
//     (loads its local copy, verifies the fingerprint)
//     READY ----------------------------->
//                              <------- SYNC (persistent renderer / audio state)
//                              <------- FRAME ... (30+ per second)
//     INPUT ----------------------------->  (key / mouse / gamepad edges; merged with every other client's)
//     PING -----------------------------> <------- PONG

#include <stdint.h>

#define NET_PROTOCOL_VERSION 1
#define NET_DEFAULT_PORT 7777

typedef enum {
    // client -> server
    NM_HELLO = 1,
    NM_READY = 2,
    NM_INPUT = 3,
    NM_PING = 4,
    NM_BYE = 5,
    NM_ERROR = 6,   // client could not load the game (string reason)
    NM_ACK = 8,     // (u32) number of FRAME messages replayed since SYNC – lets the server notice a client that falls behind
    NM_SNAPSHOT_DATA = 7, // answer to NM_SNAPSHOT_REQ: pixels of the requested surfaces

    // server -> client
    NM_GAME_INFO = 16,
    NM_SYNC = 17,
    NM_FRAME = 18,
    NM_PONG = 19,
    NM_GAME_CHANGE = 20, // the server is switching games; frames stop until the next GAME_INFO
    NM_KICK = 21,        // (string reason) connection is about to be closed
    NM_TITLE = 22,       // (string) window title
    NM_SNAPSHOT_REQ = 23, // (to an existing client) read back these surfaces for a joining client
    NM_SNAPSHOT = 24,     // (to the joining client) surface pixels, applied before the buffered frames
} NetMessageType;

// INPUT event types
typedef enum {
    NIE_KEY_DOWN = 0,
    NIE_KEY_UP = 1,
    NIE_MOUSE_DOWN = 2,
    NIE_MOUSE_UP = 3,
    NIE_WHEEL = 4,
    NIE_CHAR = 5,
} NetInputEventType;

#define NET_INPUT_HAS_WINDOW  0x01
#define NET_INPUT_HAS_EVENTS  0x02
#define NET_INPUT_HAS_MOUSE   0x04
#define NET_INPUT_HAS_PAD     0x08

#endif /* _BS_NET_PROTO_H_ */
