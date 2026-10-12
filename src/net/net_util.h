#ifndef _BS_NET_UTIL_H_
#define _BS_NET_UTIL_H_

#include "common.h"
#include <stdint.h>
#include <stddef.h>

// Cheap identity of a game file: its size plus a hash of the first / last 256 KiB and some samples in between.
// Used to make sure a client has the same data.win as the server without hashing hundreds of megabytes.
typedef struct {
    uint64_t size;
    uint64_t hash;
} NetFileFingerprint;

bool Net_fingerprintFile(const char* path, NetFileFingerprint* out);

// Parses "host", "host:port" or "[v6]:port" style addresses. Returns false for malformed input.
bool Net_parseHostPort(const char* text, char* hostOut, size_t hostCap, uint16_t* portOut, uint16_t defaultPort);

// True if `path` (after resolving "." / ".." / symlinks) lies inside the directory `root` (which may itself be given as a path
// to a file inside it: only its directory part counts when `rootIsFile`). Used to stop a hostile server from pointing the
// client at arbitrary files.
bool Net_pathInside(const char* root, const char* path);

// Replaces control characters (including ANSI escapes) so untrusted text is safe to print / put in a window title
void Net_sanitizeText(char* text);

#endif
