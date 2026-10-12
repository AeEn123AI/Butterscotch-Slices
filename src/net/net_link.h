#ifndef _BS_NET_LINK_H_
#define _BS_NET_LINK_H_

// Reliable, ordered, framed message link over TCP with optional streaming compression
// and built-in network condition simulation (latency / jitter / bandwidth) for testing.

#include "common.h"
#include "net_buf.h"
#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif
#include <winsock2.h>
typedef SOCKET NetSocket;
#define NET_INVALID_SOCKET INVALID_SOCKET
#else
typedef int NetSocket;
#define NET_INVALID_SOCKET (-1)
#endif

// ===[ Simulated network conditions ]===
// Applied to everything an endpoint SENDS. Passing the same values on both ends yields RTT = 2 * latency.
typedef struct {
    int latencyMs;
    int jitterMs;
    int bandwidthKbps; // 0 = unlimited
} NetSimConfig;

extern NetSimConfig gNetSim;

// ===[ Socket helpers ]===
bool Net_initSockets(void);
NetSocket Net_listen(uint16_t port, const char* bindAddress); // non-blocking listening socket, or NET_INVALID_SOCKET
NetSocket Net_accept(NetSocket listener); // non-blocking accepted socket or NET_INVALID_SOCKET
NetSocket Net_connect(const char* host, uint16_t port, int timeoutMs); // blocking connect with timeout, returns non-blocking socket
void Net_closeSocket(NetSocket s);
uint16_t Net_socketPort(NetSocket s);
// Waits until any socket in the list is readable (or has pending output when wantWrite[i]) or timeoutMs elapses.
void Net_waitReadable(const NetSocket* sockets, int count, int timeoutMs);

// ===[ Link ]===

typedef struct NetLinkChunk NetLinkChunk;

typedef struct {
    NetSocket fd;
    bool closed;
    char peer[64];

    // Receive
    uint8_t* rbuf;
    size_t rlen, rcap;
    NetWriter inflated; // scratch for decompressed messages
    void* inflater;     // mz_stream*

    // Send
    uint8_t* wbuf;        // bytes ready to go on the wire
    size_t wlen, wcap, wpos;
    void* deflater;       // mz_stream*
    NetLinkChunk* simHead, *simTail; // delayed chunks (simulation)
    uint64_t simLastRelease;
    uint64_t simBwNext;   // earliest time the next chunk may be released given the bandwidth limit
    size_t simQueuedBytes;
    uint64_t queuedTotal, sentTotal; // cumulative bytes put into wbuf / handed to the socket
    uint64_t* ends;       // cumulative end offsets of the whole messages in wbuf (oldest first, from endsHead)
    size_t endsCount, endsCap, endsHead;

    // Limits for untrusted peers: largest accepted message (on the wire) and largest decompressed message
    size_t maxMessage;
    size_t maxInflated;

    // Statistics (since creation)
    uint64_t statRawOut;   // payload bytes before compression
    uint64_t statWireOut;  // bytes handed to the socket
    uint64_t statWireIn;
    uint64_t statMsgsOut, statMsgsIn;
} NetLink;

NetLink* NetLink_create(NetSocket fd);
// Messages larger than this (compressed size on the wire / size after decompression) close the link. Defaults: 64 MiB / 256 MiB.
void NetLink_setLimits(NetLink* link, size_t maxMessage, size_t maxInflated);
void NetLink_destroy(NetLink* link);

// Queues a message. If compress is true the payload goes through the link's streaming deflater (both ends keep history).
void NetLink_send(NetLink* link, uint8_t type, const void* payload, size_t len, bool compress);
// Moves data between the socket and the buffers. Returns false once the link is closed/broken.
bool NetLink_pump(NetLink* link);
// Pops the next fully received message. The returned payload remains valid until the next call. Returns false if none.
bool NetLink_recv(NetLink* link, uint8_t* outType, const uint8_t** outPayload, size_t* outLen);
// Throws away everything that hasn't been sent yet, except the rest of the message that is already partly on the wire.
// Used to deliver a final message (KICK) to a client that is too far behind to ever read the backlog.
void NetLink_dropPending(NetLink* link);
// Bytes still waiting to be sent (wire buffer + simulated-delay queue).
size_t NetLink_pendingBytes(const NetLink* link);

#endif /* _BS_NET_LINK_H_ */
