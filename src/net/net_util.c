#include "net_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FP_CHUNK (256 * 1024)

static uint64_t fnv1a(uint64_t h, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

bool Net_fingerprintFile(const char* path, NetFileFingerprint* out) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long end = ftell(f);
    if (end < 0) { fclose(f); return false; }
    uint64_t size = (uint64_t) end;
    uint8_t* buf = (uint8_t*) malloc(FP_CHUNK);
    if (buf == nullptr) { fclose(f); return false; }
    uint64_t h = 0xcbf29ce484222325ull;
    // Windows of the file hashed: head, tail and 6 evenly spaced samples
    for (int i = 0; i < 8; i++) {
        uint64_t off;
        size_t len = FP_CHUNK;
        if (size <= (uint64_t) FP_CHUNK * 2) {
            if (i > 0) break;
            off = 0;
            len = (size_t) size;
        } else if (i == 0) {
            off = 0;
        } else if (i == 7) {
            off = size - FP_CHUNK;
        } else {
            off = (size - FP_CHUNK) / 7 * (uint64_t) i;
            len = 16 * 1024;
        }
        if (fseek(f, (long) off, SEEK_SET) != 0) break;
        size_t got = fread(buf, 1, len, f);
        h = fnv1a(h, buf, got);
    }
    h = fnv1a(h, (const uint8_t*) &size, sizeof(size));
    free(buf);
    fclose(f);
    out->size = size;
    out->hash = h;
    return true;
}

bool Net_parseHostPort(const char* text, char* hostOut, size_t hostCap, uint16_t* portOut, uint16_t defaultPort) {
    if (text == nullptr || text[0] == '\0') return false;
    *portOut = defaultPort;
    const char* colon = strrchr(text, ':');
    size_t hostLen;
    if (colon != nullptr && strchr(text, ':') == colon) { // exactly one colon: host:port
        hostLen = (size_t) (colon - text);
        char* end;
        long port = strtol(colon + 1, &end, 10);
        if (*end != '\0' || port <= 0 || port > 65535) return false;
        *portOut = (uint16_t) port;
    } else {
        hostLen = strlen(text);
    }
    if (hostLen == 0 || hostLen >= hostCap) return false;
    memcpy(hostOut, text, hostLen);
    hostOut[hostLen] = '\0';
    return true;
}

#ifdef _WIN32
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

static bool canonicalize(const char* in, char* out, size_t cap) {
#ifdef _WIN32
    char tmp[MAX_PATH * 2];
    DWORD n = GetFullPathNameA(in, (DWORD) sizeof(tmp), tmp, nullptr);
    if (n == 0 || n >= sizeof(tmp)) return false;
    // Normalise separators and case so the prefix comparison below works
    for (char* p = tmp; *p; p++) {
        if (*p == '/') *p = '\\';
        *p = (char) tolower((unsigned char) *p);
    }
    snprintf(out, cap, "%s", tmp);
    return true;
#else
    char tmp[PATH_MAX];
    if (realpath(in, tmp) == nullptr) return false;
    snprintf(out, cap, "%s", tmp);
    return true;
#endif
}

bool Net_pathInside(const char* root, const char* path) {
    char r[4096], p[4096];
    if (!canonicalize(root, r, sizeof(r)) || !canonicalize(path, p, sizeof(p))) return false;
    size_t rl = strlen(r);
    while (rl > 1 && (r[rl - 1] == '/' || r[rl - 1] == '\\')) r[--rl] = '\0';
    if (strncmp(p, r, rl) != 0) return false;
    return p[rl] == '/' || p[rl] == '\\' || p[rl] == '\0';
}

void Net_sanitizeText(char* text) {
    for (unsigned char* p = (unsigned char*) text; *p; p++) {
        if (*p < 32 || *p == 127) *p = '?';
    }
}
