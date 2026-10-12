#ifndef _BS_NET_RENDER_H_
#define _BS_NET_RENDER_H_

// Network renderer.
//
// Server side: RemoteRenderer implements the Renderer vtable by serializing every draw / resource call into a compact
// command stream (net_buf.h encoding), while keeping enough shadow state to answer every query locally (surface sizes,
// blend state, texture handles, ...). The game logic therefore never has to wait for a client.
//
// Client side: NetRenderDecoder replays a command stream on a real Renderer (OpenGL), translating the server's
// surface / sprite / shader IDs to local ones.

#include "common.h"
#include "net_buf.h"
#include "renderer.h"
#include "runner.h"

// ===[ Server: recording renderer ]===

Renderer* RemoteRenderer_create(void);

// The pending command stream. The server appends NROP_FRAME_END, sends it and then resets it.
NetWriter* RemoteRenderer_commands(Renderer* renderer);
// Appends the start-of-frame marker (the client clears its window there).
void RemoteRenderer_beginFrame(Renderer* renderer);
// Appends the end-of-frame marker.
void RemoteRenderer_endFrame(Renderer* renderer);
// Statistics: number of renderer calls recorded (all, excluding state deltas)
uint64_t RemoteRenderer_callCount(Renderer* renderer);
// Emits the persistent resource state (surfaces, dynamic sprites, uniform handle definitions) a joining client needs.
// Also forces all delta-coded state to be re-sent on the next call.
void RemoteRenderer_writeJoinState(Renderer* renderer, NetWriter* out);
// Ids of the surfaces that currently exist (the client reads their pixels for a late joiner). Returns the count written (<= max).
int RemoteRenderer_liveSurfaces(Renderer* renderer, int32_t* out, int max);
// Marks the delta-coded state dirty, so the next frame re-sends it in full (used when a new client attaches).
void RemoteRenderer_invalidateSyncedState(Renderer* renderer);
// Virtual (server) window size the game is told it renders into
void RemoteRenderer_setCapabilities(Renderer* renderer, bool shadersSupported);

// ===[ Client: replaying decoder ]===

typedef struct NetRenderDecoder NetRenderDecoder;

NetRenderDecoder* NetRenderDecoder_create(Renderer* real, Runner* clientRunner);
void NetRenderDecoder_destroy(NetRenderDecoder* decoder);
// The client's real framebuffer size (substituted for the server's virtual window when the server addresses the window)
void NetRenderDecoder_setWindowSize(NetRenderDecoder* decoder, int32_t windowW, int32_t windowH);
// Called when a frame starts (before any of its draw commands): the client clears the host framebuffer here
void NetRenderDecoder_setFrameBeginHook(NetRenderDecoder* decoder, void (*hook)(void* userData), void* userData);
// Replays commands until the end of the buffer. Sets *outFrameEnd if a frame-end marker was reached. Returns false on a protocol error.
bool NetRenderDecoder_run(NetRenderDecoder* decoder, const uint8_t* data, size_t len, bool* outFrameEnd);

// Late join support
// Donor: reads back the surfaces listed in a SNAPSHOT_REQ payload into a SNAPSHOT_DATA payload.
bool NetRenderDecoder_buildSnapshot(NetRenderDecoder* decoder, const uint8_t* request, size_t len, NetWriter* out);
// Joiner: uploads the pixels of a SNAPSHOT payload into the (already created) surfaces. Returns the number of surfaces restored.
int NetRenderDecoder_applySnapshot(NetRenderDecoder* decoder, const uint8_t* data, size_t len, bool verify);

#endif /* _BS_NET_RENDER_H_ */
