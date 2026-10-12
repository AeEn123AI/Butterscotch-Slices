#ifndef _BS_NET_BUF_H_
#define _BS_NET_BUF_H_

// Compact, endian-independent, lossless binary serialization used by the client/server protocol.
//
// - Integers are LEB128 varints (signed values are zig-zag encoded).
// - Floats are lossless: integral values and multiples of 1/16 take 1-3 bytes, everything else takes 5.
// - Strings are length-prefixed.

#include "common.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

// ===[ Writer ]===

typedef struct {
    uint8_t* data;
    size_t len;
    size_t cap;
} NetWriter;

static inline void NetWriter_init(NetWriter* w) {
    w->data = nullptr;
    w->len = 0;
    w->cap = 0;
}

static inline void NetWriter_free(NetWriter* w) {
    free(w->data);
    w->data = nullptr;
    w->len = w->cap = 0;
}

static inline void NetWriter_reset(NetWriter* w) {
    w->len = 0;
}

static inline void NetWriter_reserve(NetWriter* w, size_t extra) {
    if (w->len + extra <= w->cap) return;
    size_t newCap = w->cap ? w->cap : 4096;
    while (newCap < w->len + extra) newCap *= 2;
    uint8_t* p = (uint8_t*) realloc(w->data, newCap);
    if (p == nullptr) abort();
    w->data = p;
    w->cap = newCap;
}

static inline void NetWriter_u8(NetWriter* w, uint8_t v) {
    NetWriter_reserve(w, 1);
    w->data[w->len++] = v;
}

static inline void NetWriter_bytes(NetWriter* w, const void* src, size_t n) {
    if (n == 0) return;
    NetWriter_reserve(w, n);
    memcpy(w->data + w->len, src, n);
    w->len += n;
}

static inline void NetWriter_u32(NetWriter* w, uint32_t v) {
    NetWriter_reserve(w, 5);
    while (v >= 0x80) {
        w->data[w->len++] = (uint8_t) (v | 0x80);
        v >>= 7;
    }
    w->data[w->len++] = (uint8_t) v;
}

static inline void NetWriter_u64(NetWriter* w, uint64_t v) {
    NetWriter_reserve(w, 10);
    while (v >= 0x80) {
        w->data[w->len++] = (uint8_t) (v | 0x80);
        v >>= 7;
    }
    w->data[w->len++] = (uint8_t) v;
}

static inline uint32_t NetZigZag32(int32_t v) {
    return ((uint32_t) v << 1) ^ (uint32_t) (v >> 31);
}

static inline int32_t NetUnZigZag32(uint32_t v) {
    return (int32_t) (v >> 1) ^ -(int32_t) (v & 1);
}

static inline void NetWriter_i32(NetWriter* w, int32_t v) {
    NetWriter_u32(w, NetZigZag32(v));
}

static inline void NetWriter_fixed32(NetWriter* w, uint32_t v) {
    uint8_t b[4] = { (uint8_t) v, (uint8_t) (v >> 8), (uint8_t) (v >> 16), (uint8_t) (v >> 24) };
    NetWriter_bytes(w, b, 4);
}

static inline void NetWriter_fixed64(NetWriter* w, uint64_t v) {
    NetWriter_fixed32(w, (uint32_t) v);
    NetWriter_fixed32(w, (uint32_t) (v >> 32));
}

static inline uint32_t NetFloatBits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static inline float NetBitsFloat(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}

// Lossless float coding. Low 2 bits of the varint select the form:
//   0: integer (value >> 2, zig-zag)
//   1: multiple of 1/16 (value >> 2, zig-zag, divided by 16)
//   2: raw IEEE-754 bits follow as 4 bytes
static inline void NetWriter_f32(NetWriter* w, float f) {
    // Range check keeps the int conversion defined (and rejects NaN, since the comparisons are false)
    if (f > -8000000.0f && f < 8000000.0f) {
        int32_t i = (int32_t) f;
        if ((float) i == f && !(i == 0 && NetFloatBits(f) != 0)) {
            NetWriter_u32(w, NetZigZag32(i) << 2);
            return;
        }
        float scaled = f * 16.0f;
        if (scaled > -8000000.0f && scaled < 8000000.0f) {
            int32_t s = (int32_t) scaled;
            if ((float) s == scaled && (float) s / 16.0f == f && !(s == 0 && NetFloatBits(f) != 0)) {
                NetWriter_u32(w, (NetZigZag32(s) << 2) | 1);
                return;
            }
        }
    }
    NetWriter_u32(w, 2);
    NetWriter_fixed32(w, NetFloatBits(f));
}

static inline void NetWriter_bool(NetWriter* w, bool b) {
    NetWriter_u8(w, b ? 1 : 0);
}

static inline void NetWriter_str(NetWriter* w, const char* s) {
    if (s == nullptr) {
        NetWriter_u32(w, 0);
        return;
    }
    size_t n = strlen(s);
    NetWriter_u32(w, (uint32_t) n + 1);
    NetWriter_bytes(w, s, n);
}

// ===[ Reader ]===

typedef struct {
    const uint8_t* data;
    size_t len;
    size_t pos;
    bool error;
} NetReader;

static inline void NetReader_init(NetReader* r, const void* data, size_t len) {
    r->data = (const uint8_t*) data;
    r->len = len;
    r->pos = 0;
    r->error = false;
}

static inline bool NetReader_eof(const NetReader* r) {
    return r->pos >= r->len;
}

static inline uint8_t NetReader_u8(NetReader* r) {
    if (r->pos >= r->len) {
        r->error = true;
        return 0;
    }
    return r->data[r->pos++];
}

static inline uint32_t NetReader_u32(NetReader* r) {
    uint32_t v = 0;
    int shift = 0;
    while (true) {
        uint8_t b = NetReader_u8(r);
        if (r->error) return 0;
        if (shift > 28) { // a 32-bit value never needs a 6th byte (and shifting by 35 would be undefined)
            r->error = true;
            return 0;
        }
        v |= (uint32_t) (b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return v;
}

static inline uint64_t NetReader_u64(NetReader* r) {
    uint64_t v = 0;
    int shift = 0;
    while (true) {
        uint8_t b = NetReader_u8(r);
        if (r->error) return 0;
        if (shift > 63) {
            r->error = true;
            return 0;
        }
        v |= (uint64_t) (b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return v;
}

static inline int32_t NetReader_i32(NetReader* r) {
    return NetUnZigZag32(NetReader_u32(r));
}

static inline uint32_t NetReader_fixed32(NetReader* r) {
    uint32_t b0 = NetReader_u8(r), b1 = NetReader_u8(r), b2 = NetReader_u8(r), b3 = NetReader_u8(r);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

static inline uint64_t NetReader_fixed64(NetReader* r) {
    uint64_t lo = NetReader_fixed32(r);
    uint64_t hi = NetReader_fixed32(r);
    return lo | (hi << 32);
}

static inline float NetReader_f32(NetReader* r) {
    uint32_t v = NetReader_u32(r);
    switch (v & 3) {
        case 0: return (float) NetUnZigZag32(v >> 2);
        case 1: return (float) NetUnZigZag32(v >> 2) / 16.0f;
        default: return NetBitsFloat(NetReader_fixed32(r));
    }
}

static inline bool NetReader_bool(NetReader* r) {
    return NetReader_u8(r) != 0;
}

// Returns a pointer into the reader's buffer (not NUL terminated in the buffer) – copies into a malloc'd string.
// Returns nullptr for the encoded-null string.
static inline char* NetReader_strDup(NetReader* r) {
    uint32_t n = NetReader_u32(r);
    if (n == 0 || r->error) return nullptr;
    n--;
    if (n > r->len - r->pos) {
        r->error = true;
        return nullptr;
    }
    char* s = (char*) malloc((size_t) n + 1);
    if (s == nullptr) abort();
    memcpy(s, r->data + r->pos, n);
    s[n] = '\0';
    r->pos += n;
    return s;
}

static inline const uint8_t* NetReader_bytes(NetReader* r, size_t n) {
    if (n > r->len - r->pos) {
        r->error = true;
        return nullptr;
    }
    const uint8_t* p = r->data + r->pos;
    r->pos += n;
    return p;
}

#endif /* _BS_NET_BUF_H_ */
