#include "net_link.h"

#include "gettime.h"
#include "log.h"
#include "miniz.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>

#ifdef _WIN32
#include <ws2tcpip.h>
#define NET_ERRNO WSAGetLastError()
#define NET_WOULDBLOCK(e) ((e) == WSAEWOULDBLOCK)
#define NET_INTR(e) (0)
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#define NET_ERRNO errno
#define NET_WOULDBLOCK(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
#define NET_INTR(e) ((e) == EINTR)
#endif

NetSimConfig gNetSim = {0, 0, 0};

// Message flag: payload is part of the compressed stream
#define NET_MSG_COMPRESSED 0x80

// ===[ Sockets ]===

bool Net_initSockets(void) {
#ifdef _WIN32
    static bool done = false;
    if (done) return true;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    done = true;
#else
    static bool done = false;
    if (!done) {
        // A peer closing a connection must not kill the process
        signal(SIGPIPE, SIG_IGN);
        done = true;
    }
#endif
    return true;
}

static void setNonBlocking(NetSocket s) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

static void setNoDelay(NetSocket s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*) &one, sizeof(one));
}

void Net_closeSocket(NetSocket s) {
    if (s == NET_INVALID_SOCKET) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

NetSocket Net_listen(uint16_t port, const char* bindAddress) {
    if (!Net_initSockets()) return NET_INVALID_SOCKET;
    NetSocket s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == NET_INVALID_SOCKET) return s;
#ifndef _WIN32
    // Allow quick restarts on the same port (on Windows SO_REUSEADDR would allow port hijacking, so it's not set there)
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*) &one, sizeof(one));
#endif
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = (bindAddress != nullptr && bindAddress[0] != '\0') ? inet_addr(bindAddress) : htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr*) &addr, sizeof(addr)) != 0 || listen(s, 16) != 0) {
        Net_closeSocket(s);
        return NET_INVALID_SOCKET;
    }
    setNonBlocking(s);
    return s;
}

uint16_t Net_socketPort(NetSocket s) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getsockname(s, (struct sockaddr*) &addr, &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

NetSocket Net_accept(NetSocket listener) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    NetSocket s = accept(listener, (struct sockaddr*) &addr, &len);
    if (s == NET_INVALID_SOCKET) return s;
    setNonBlocking(s);
    setNoDelay(s);
    return s;
}

NetSocket Net_connect(const char* host, uint16_t port, int timeoutMs) {
    if (!Net_initSockets()) return NET_INVALID_SOCKET;
    struct addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%u", (unsigned) port);
    if (getaddrinfo(host, portStr, &hints, &res) != 0 || res == nullptr) return NET_INVALID_SOCKET;

    NetSocket result = NET_INVALID_SOCKET;
    for (struct addrinfo* ai = res; ai != nullptr && result == NET_INVALID_SOCKET; ai = ai->ai_next) {
        NetSocket s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == NET_INVALID_SOCKET) continue;
        setNonBlocking(s);
        int rc = connect(s, ai->ai_addr, (int) ai->ai_addrlen);
        bool ok = (rc == 0);
        if (!ok) {
            int e = NET_ERRNO;
#ifdef _WIN32
            bool inProgress = NET_WOULDBLOCK(e);
#else
            bool inProgress = (e == EINPROGRESS);
#endif
            if (inProgress) {
                fd_set wfds, efds;
                FD_ZERO(&wfds);
                FD_ZERO(&efds);
                FD_SET(s, &wfds);
                FD_SET(s, &efds);
                struct timeval tv;
                tv.tv_sec = timeoutMs / 1000;
                tv.tv_usec = (timeoutMs % 1000) * 1000;
                if (select((int) s + 1, nullptr, &wfds, &efds, &tv) > 0 && FD_ISSET(s, &wfds)) {
                    int soErr = 0;
                    socklen_t sl = sizeof(soErr);
                    getsockopt(s, SOL_SOCKET, SO_ERROR, (char*) &soErr, &sl);
                    ok = (soErr == 0);
                }
            }
        }
        if (ok) {
            setNoDelay(s);
            result = s;
        } else {
            Net_closeSocket(s);
        }
    }
    freeaddrinfo(res);
    return result;
}

void Net_waitReadable(const NetSocket* sockets, int count, int timeoutMs) {
    if (timeoutMs < 0) timeoutMs = 0;
    if (count <= 0) {
        if (timeoutMs > 0) {
#ifdef _WIN32
            Sleep((DWORD) timeoutMs);
#else
            usleep((useconds_t) timeoutMs * 1000);
#endif
        }
        return;
    }
    fd_set rfds;
    FD_ZERO(&rfds);
    int maxFd = 0;
    for (int i = 0; i < count; i++) {
        if (sockets[i] == NET_INVALID_SOCKET) continue;
        FD_SET(sockets[i], &rfds);
        if ((int) sockets[i] > maxFd) maxFd = (int) sockets[i];
    }
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    select(maxFd + 1, &rfds, nullptr, nullptr, &tv);
}

// ===[ Link ]===

struct NetLinkChunk {
    NetLinkChunk* next;
    uint64_t release; // nanos
    size_t len;
    uint8_t* data;    // points just behind the struct (single allocation)
};

NetLink* NetLink_create(NetSocket fd) {
    NetLink* link = (NetLink*) calloc(1, sizeof(NetLink));
    if (link == nullptr) abort();
    link->fd = fd;
    NetWriter_init(&link->inflated);
    link->maxMessage = 64u << 20;
    link->maxInflated = 256u << 20;
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getpeername(fd, (struct sockaddr*) &addr, &len) == 0) {
        snprintf(link->peer, sizeof(link->peer), "%s:%u", inet_ntoa(addr.sin_addr), (unsigned) ntohs(addr.sin_port));
    } else {
        snprintf(link->peer, sizeof(link->peer), "?");
    }
    return link;
}

void NetLink_setLimits(NetLink* link, size_t maxMessage, size_t maxInflated) {
    link->maxMessage = maxMessage;
    link->maxInflated = maxInflated;
}

void NetLink_destroy(NetLink* link) {
    if (link == nullptr) return;
    Net_closeSocket(link->fd);
    free(link->rbuf);
    free(link->wbuf);
    free(link->ends);
    NetWriter_free(&link->inflated);
    if (link->inflater) {
        mz_inflateEnd((mz_streamp) link->inflater);
        free(link->inflater);
    }
    if (link->deflater) {
        mz_deflateEnd((mz_streamp) link->deflater);
        free(link->deflater);
    }
    NetLinkChunk* c = link->simHead;
    while (c) {
        NetLinkChunk* n = c->next;
        free(c);
        c = n;
    }
    free(link);
}

static void wbufAppend(NetLink* link, const void* data, size_t n) {
    // Compact consumed prefix
    if (link->wpos > 0 && link->wpos == link->wlen) {
        link->wpos = link->wlen = 0;
    }
    if (link->wlen + n > link->wcap) {
        if (link->wpos > 0) {
            memmove(link->wbuf, link->wbuf + link->wpos, link->wlen - link->wpos);
            link->wlen -= link->wpos;
            link->wpos = 0;
        }
        if (link->wlen + n > link->wcap) {
            size_t cap = link->wcap ? link->wcap : 8192;
            while (cap < link->wlen + n) cap *= 2;
            link->wbuf = (uint8_t*) realloc(link->wbuf, cap);
            if (link->wbuf == nullptr) abort();
            link->wcap = cap;
        }
    }
    memcpy(link->wbuf + link->wlen, data, n);
    link->wlen += n;

    // Remember where this whole message ends (see NetLink_dropPending)
    link->queuedTotal += n;
    if (link->endsHead > 0 && link->endsHead == link->endsCount) link->endsHead = link->endsCount = 0;
    if (link->endsCount == link->endsCap) {
        if (link->endsHead > 0) {
            memmove(link->ends, link->ends + link->endsHead, (link->endsCount - link->endsHead) * sizeof(uint64_t));
            link->endsCount -= link->endsHead;
            link->endsHead = 0;
        } else {
            link->endsCap = link->endsCap ? link->endsCap * 2 : 64;
            link->ends = (uint64_t*) realloc(link->ends, link->endsCap * sizeof(uint64_t));
            if (link->ends == nullptr) abort();
        }
    }
    link->ends[link->endsCount++] = link->queuedTotal;
}

static uint64_t simRandomJitterNs(void) {
    if (gNetSim.jitterMs <= 0) return 0;
    static uint32_t state = 0x1234567u;
    state = state * 1664525u + 1013904223u;
    return (uint64_t) ((state >> 8) % (uint32_t) (gNetSim.jitterMs * 1000 + 1)) * 1000ull;
}

// Hands a fully framed message to the wire (possibly through the simulation delay queue)
static void emitFramed(NetLink* link, const uint8_t* data, size_t n) {
    bool simulate = gNetSim.latencyMs > 0 || gNetSim.jitterMs > 0 || gNetSim.bandwidthKbps > 0;
    if (!simulate && link->simHead == nullptr) {
        wbufAppend(link, data, n);
        return;
    }
    uint64_t now = nowNanos();
    uint64_t release = now + (uint64_t) gNetSim.latencyMs * 1000000ull + simRandomJitterNs();
    if (release < link->simLastRelease) release = link->simLastRelease; // TCP never reorders
    link->simLastRelease = release;
    NetLinkChunk* c = (NetLinkChunk*) malloc(sizeof(NetLinkChunk) + n);
    if (c == nullptr) abort();
    c->data = (uint8_t*) (c + 1);
    c->next = nullptr;
    c->release = release;
    c->len = n;
    memcpy(c->data, data, n);
    if (link->simTail) link->simTail->next = c; else link->simHead = c;
    link->simTail = c;
    link->simQueuedBytes += n;
}

static void simRelease(NetLink* link) {
    uint64_t now = nowNanos();
    while (link->simHead != nullptr) {
        NetLinkChunk* c = link->simHead;
        if (c->release > now) break;
        if (gNetSim.bandwidthKbps > 0) {
            if (link->simBwNext > now) break;
            uint64_t bytesPerSec = (uint64_t) gNetSim.bandwidthKbps * 125ull;
            link->simBwNext = now + (uint64_t) c->len * 1000000000ull / bytesPerSec;
        }
        wbufAppend(link, c->data, c->len);
        link->simHead = c->next;
        if (link->simHead == nullptr) link->simTail = nullptr;
        link->simQueuedBytes -= c->len;
        free(c);
    }
}

void NetLink_send(NetLink* link, uint8_t type, const void* payload, size_t len, bool compress) {
    if (link->closed) return;
    link->statRawOut += len;
    link->statMsgsOut++;

    NetWriter msg;
    NetWriter_init(&msg);
    const uint8_t* body = (const uint8_t*) payload;
    size_t bodyLen = len;
    uint8_t* compressed = nullptr;

    if (compress) {
        if (link->deflater == nullptr) {
            mz_streamp s = (mz_streamp) calloc(1, sizeof(mz_stream));
            if (mz_deflateInit(s, 6) != MZ_OK) abort();
            link->deflater = s;
        }
        mz_streamp s = (mz_streamp) link->deflater;
        size_t bound = (size_t) mz_deflateBound(s, (mz_ulong) len) + 64;
        compressed = (uint8_t*) malloc(bound);
        if (compressed == nullptr) abort();
        s->next_in = (const unsigned char*) payload;
        s->avail_in = (unsigned int) len;
        s->next_out = compressed;
        s->avail_out = (unsigned int) bound;
        int rc = mz_deflate(s, MZ_SYNC_FLUSH);
        if (rc != MZ_OK) {
            logError("Net: deflate failed (%d)\n", rc);
            free(compressed);
            link->closed = true;
            return;
        }
        body = compressed;
        bodyLen = bound - s->avail_out;
        type |= NET_MSG_COMPRESSED;
    }

    // Frame: varint(length of type+body), type byte, body
    NetWriter_u32(&msg, (uint32_t) (bodyLen + 1));
    NetWriter_u8(&msg, type);
    NetWriter_bytes(&msg, body, bodyLen);
    emitFramed(link, msg.data, msg.len);
    NetWriter_free(&msg);
    free(compressed);
}

bool NetLink_pump(NetLink* link) {
    if (link->closed) return false;

    simRelease(link);

    // Write
    while (link->wpos < link->wlen) {
        int n = (int) send(link->fd, (const char*) link->wbuf + link->wpos, (int) (link->wlen - link->wpos),
#ifdef MSG_NOSIGNAL
            MSG_NOSIGNAL
#else
            0
#endif
        );
        if (n > 0) {
            link->wpos += (size_t) n;
            link->statWireOut += (uint64_t) n;
            link->sentTotal += (uint64_t) n;
            while (link->endsHead < link->endsCount && link->ends[link->endsHead] <= link->sentTotal) link->endsHead++;
        } else {
            int e = NET_ERRNO;
            if (n < 0 && (NET_WOULDBLOCK(e) || NET_INTR(e))) break;
            link->closed = true;
            return false;
        }
    }
    if (link->wpos == link->wlen) link->wpos = link->wlen = 0;

    // Read
    while (true) {
        if (link->rlen + 4096 > link->rcap) {
            size_t cap = link->rcap ? link->rcap * 2 : 16384;
            link->rbuf = (uint8_t*) realloc(link->rbuf, cap);
            if (link->rbuf == nullptr) abort();
            link->rcap = cap;
        }
        int n = (int) recv(link->fd, (char*) link->rbuf + link->rlen, (int) (link->rcap - link->rlen), 0);
        if (n > 0) {
            link->rlen += (size_t) n;
            link->statWireIn += (uint64_t) n;
            if ((size_t) n < link->rcap - link->rlen + (size_t) n) break; // drained
        } else if (n == 0) {
            link->closed = true;
            return false;
        } else {
            int e = NET_ERRNO;
            if (NET_WOULDBLOCK(e) || NET_INTR(e)) break;
            link->closed = true;
            return false;
        }
    }
    return true;
}

bool NetLink_recv(NetLink* link, uint8_t* outType, const uint8_t** outPayload, size_t* outLen) {
    if (link->rlen == 0) return false;
    NetReader r;
    NetReader_init(&r, link->rbuf, link->rlen);
    uint32_t msgLen = NetReader_u32(&r);
    if (r.error) return false; // partial varint
    if (msgLen == 0 || msgLen > link->maxMessage) {
        logError("Net: invalid message length %u from %s\n", msgLen, link->peer);
        link->closed = true;
        return false;
    }
    if ((size_t) msgLen > link->rlen - r.pos) return false;
    uint8_t type = link->rbuf[r.pos];
    const uint8_t* body = link->rbuf + r.pos + 1;
    size_t bodyLen = msgLen - 1;
    size_t consumed = r.pos + msgLen;

    static uint8_t* heldBuf = nullptr; // keeps the last returned message alive across the memmove below
    static size_t heldCap = 0;

    const uint8_t* outBody = body;
    size_t outBodyLen = bodyLen;
    if (type & NET_MSG_COMPRESSED) {
        type &= (uint8_t) ~NET_MSG_COMPRESSED;
        if (link->inflater == nullptr) {
            mz_streamp s = (mz_streamp) calloc(1, sizeof(mz_stream));
            if (mz_inflateInit(s) != MZ_OK) abort();
            link->inflater = s;
        }
        mz_streamp s = (mz_streamp) link->inflater;
        NetWriter_reset(&link->inflated);
        s->next_in = body;
        s->avail_in = (unsigned int) bodyLen;
        while (true) {
            if (link->inflated.len > link->maxInflated) {
                logError("Net: decompressed message from %s exceeds %lu bytes\n", link->peer, (unsigned long) link->maxInflated);
                link->closed = true;
                return false;
            }
            NetWriter_reserve(&link->inflated, 65536);
            s->next_out = link->inflated.data + link->inflated.len;
            s->avail_out = (unsigned int) (link->inflated.cap - link->inflated.len);
            int rc = mz_inflate(s, MZ_SYNC_FLUSH);
            link->inflated.len = link->inflated.cap - s->avail_out;
            if (rc == MZ_OK || rc == MZ_BUF_ERROR) {
                if (s->avail_in == 0 && s->avail_out != 0) break;
                if (rc == MZ_BUF_ERROR && s->avail_in == 0) break;
                continue;
            }
            if (rc == MZ_STREAM_END) break;
            logError("Net: inflate failed (%d) from %s\n", rc, link->peer);
            link->closed = true;
            return false;
        }
        outBody = link->inflated.data;
        outBodyLen = link->inflated.len;
    }

    // Uncompressed messages live in rbuf, which we compact now – keep a copy so the pointer stays valid
    if (outBody == body) {
        if (heldCap < outBodyLen) {
            heldCap = outBodyLen + 1024;
            heldBuf = (uint8_t*) realloc(heldBuf, heldCap);
            if (heldBuf == nullptr) abort();
        }
        memcpy(heldBuf, outBody, outBodyLen);
        outBody = heldBuf;
    }
    memmove(link->rbuf, link->rbuf + consumed, link->rlen - consumed);
    link->rlen -= consumed;

    link->statMsgsIn++;
    *outType = type;
    *outPayload = outBody;
    *outLen = outBodyLen;
    return true;
}

void NetLink_dropPending(NetLink* link) {
    // Finish the message that is already partly sent, drop everything queued behind it
    size_t keep = link->wpos;
    if (link->endsHead < link->endsCount && link->ends[link->endsHead] > link->sentTotal) {
        keep = link->wpos + (size_t) (link->ends[link->endsHead] - link->sentTotal);
        if (keep > link->wlen) keep = link->wlen;
        link->endsCount = link->endsHead + 1;
        link->queuedTotal = link->ends[link->endsHead];
    } else {
        link->endsCount = link->endsHead = 0;
        link->queuedTotal = link->sentTotal;
    }
    link->wlen = keep;
    NetLinkChunk* c = link->simHead;
    while (c) {
        NetLinkChunk* n = c->next;
        free(c);
        c = n;
    }
    link->simHead = link->simTail = nullptr;
    link->simQueuedBytes = 0;
}

size_t NetLink_pendingBytes(const NetLink* link) {
    return (link->wlen - link->wpos) + link->simQueuedBytes;
}
