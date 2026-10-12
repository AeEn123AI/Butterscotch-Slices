#ifndef _BS_GAME_SCAN_H_
#define _BS_GAME_SCAN_H_

// Finds GameMaker games (data.win / game.unx / game.ios / game.droid) on this machine: Steam / GOG libraries, the
// working directory, the executable's directory and shallow searches of common folders.

#include "common.h"
#include <stdint.h>
#include <stddef.h>

typedef struct {
    char* path;   // full path to the data file
    char* title;  // display name read from the file (or the folder name if it could not be read)
    uint64_t size;
} DetectedGame;

// Scans the usual places. Returns a malloc'd array (free with GameScan_free). `extraRoot` (may be null) is searched too.
int GameScan_detect(DetectedGame** out, const char* extraRoot);
void GameScan_free(DetectedGame* games, int count);
// Fills in the title / size of a single data file the user chose. Returns false if it doesn't look like a game file.
bool GameScan_describe(const char* path, DetectedGame* out);
// Finds, among `games`, the entry whose fingerprint (size + hash) matches. Returns the index or -1.
int GameScan_findByFingerprint(const DetectedGame* games, int count, uint64_t size, uint64_t hash);
// Is `name` one of the file names a GameMaker runner loads?
bool GameScan_isGameFileName(const char* name);

#endif
