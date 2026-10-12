#include "game_scan.h"

#include "net_util.h"
#include "data_win.h"
#include "log.h"
#include "utils.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct {
    DetectedGame* items;
    int count, cap;
    int budget; // remaining directory entries we are willing to look at
} ScanState;

static bool strEqualNoCase(const char* a, const char* b) {
    while (*a && *b) {
        if (tolower((unsigned char) *a) != tolower((unsigned char) *b)) return false;
        a++; b++;
    }
    return *a == *b;
}

bool GameScan_isGameFileName(const char* name) {
    return strEqualNoCase(name, "data.win") || strEqualNoCase(name, "game.unx") ||
           strEqualNoCase(name, "game.ios") || strEqualNoCase(name, "game.droid");
}

static char* joinPath(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    char* r = (char*) safeMalloc(la + lb + 2);
    memcpy(r, a, la);
    size_t n = la;
    if (la > 0 && a[la - 1] != '/' && a[la - 1] != '\\') r[n++] = '/';
    memcpy(r + n, b, lb + 1);
    return r;
}

static const char* lastComponent(const char* path) {
    const char* p = strrchr(path, '/');
    const char* q = strrchr(path, '\\');
    if (q != nullptr && (p == nullptr || q > p)) p = q;
    return p != nullptr ? p + 1 : path;
}

// Name of the folder containing `path`
static char* parentFolderName(const char* path) {
    char* dir = safeStrdup(path);
    bsGetDirname(dir);
    char* name = safeStrdup(lastComponent(dir));
    free(dir);
    return name;
}

bool GameScan_describe(const char* path, DetectedGame* out) {
    memset(out, 0, sizeof(*out));
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return false;
    char magic[4] = {0};
    size_t got = fread(magic, 1, 4, f);
    fclose(f);
    if (got != 4 || memcmp(magic, "FORM", 4) != 0) return false;

    NetFileFingerprint fp;
    if (!Net_fingerprintFile(path, &fp)) return false;
    out->path = safeStrdup(path);
    out->size = fp.size;

    // Read the display name from the GEN8 chunk without loading the whole file
    DataWinParserOptions options = {0};
    options.parseGen8 = true;
    options.parseStrg = true; // GEN8 only holds references into the string table
    options.loadType = DATAWINLOADTYPE_MAP_FILE;
    DataWin* dw = DataWin_parse(path, options);
    if (dw != nullptr) {
        const char* name = dw->gen8.displayName != nullptr && dw->gen8.displayName[0] != '\0' ? dw->gen8.displayName : dw->gen8.name;
        if (name != nullptr && name[0] != '\0') out->title = safeStrdup(name);
        DataWin_free(dw);
    }
    if (out->title == nullptr) out->title = parentFolderName(path);
    return true;
}

static bool alreadyHave(ScanState* st, const char* path) {
    for (int i = 0; i < st->count; i++) {
        if (strcmp(st->items[i].path, path) == 0) return true;
    }
    return false;
}

static void addGame(ScanState* st, const char* path) {
    if (alreadyHave(st, path)) return;
    DetectedGame g;
    if (!GameScan_describe(path, &g)) return;
    // The same file reached through two spellings of the path (symlinks etc.): identical size + title + folder name
    for (int i = 0; i < st->count; i++) {
        if (st->items[i].size == g.size && strcmp(st->items[i].title, g.title) == 0) {
            char* a = parentFolderName(st->items[i].path);
            char* b = parentFolderName(g.path);
            bool same = strcmp(a, b) == 0;
            free(a); free(b);
            if (same) { free(g.path); free(g.title); return; }
        }
    }
    if (st->count == st->cap) {
        st->cap = st->cap ? st->cap * 2 : 16;
        st->items = (DetectedGame*) safeRealloc(st->items, (size_t) st->cap * sizeof(DetectedGame));
    }
    st->items[st->count++] = g;
}

static bool skipDirName(const char* name) {
    static const char* const skip[] = { "node_modules", "proc", "sys", "dev", "AppData", "Library", "snap", "System32", "Windows", "$Recycle.Bin", "cache", ".cache", "target", "build", nullptr };
    for (int i = 0; skip[i] != nullptr; i++) if (strEqualNoCase(name, skip[i])) return true;
    return false;
}

#ifdef _WIN32
static void scanDir(ScanState* st, const char* dir, int depth) {
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (st->budget-- <= 0) break;
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char* full = joinPath(dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0 && !skipDirName(fd.cFileName)) scanDir(st, full, depth - 1);
        } else if (GameScan_isGameFileName(fd.cFileName)) {
            addGame(st, full);
        }
        free(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
#else
static bool endsWithNoCase(const char* s, const char* suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strEqualNoCase(s + a - b, suffix);
}

static void scanDir(ScanState* st, const char* dir, int depth) {
    DIR* d = opendir(dir);
    if (d == nullptr) return;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (st->budget-- <= 0) break;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char* full = joinPath(dir, e->d_name);
        struct stat sb;
        if (stat(full, &sb) == 0) {
            if (S_ISDIR(sb.st_mode)) {
                if (endsWithNoCase(e->d_name, ".app")) {
                    // macOS application bundle: game files are in Contents/Resources
                    char* res = joinPath(full, "Contents/Resources");
                    scanDir(st, res, 0);
                    free(res);
                } else if (depth > 0 && e->d_name[0] != '.' && !skipDirName(e->d_name)) {
                    scanDir(st, full, depth - 1);
                }
            } else if (S_ISREG(sb.st_mode) && GameScan_isGameFileName(e->d_name)) {
                addGame(st, full);
            }
        }
        free(full);
    }
    closedir(d);
}
#endif

static bool dirExists(const char* path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat sb;
    return stat(path, &sb) == 0 && S_ISDIR(sb.st_mode);
#endif
}

// Reads extra Steam library folders out of libraryfolders.vdf: lines like   "path"		"/mnt/games/SteamLibrary"
static void scanSteamLibraries(ScanState* st, const char* steamRoot) {
    char* vdf = joinPath(steamRoot, "steamapps/libraryfolders.vdf");
    FILE* f = fopen(vdf, "rb");
    free(vdf);
    if (f == nullptr) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char* p = strstr(line, "\"path\"");
        if (p == nullptr) continue;
        p += 6;
        char* q1 = strchr(p, '"');
        if (q1 == nullptr) continue;
        char* q2 = strchr(q1 + 1, '"');
        if (q2 == nullptr) continue;
        *q2 = '\0';
        // Unescape doubled backslashes (Windows paths)
        char* path = safeStrdup(q1 + 1);
        char* w = path;
        for (char* r = path; *r; r++) {
            if (r[0] == '\\' && r[1] == '\\') r++;
            *w++ = *r;
        }
        *w = '\0';
        char* common = joinPath(path, "steamapps/common");
        if (dirExists(common)) scanDir(st, common, 3);
        free(common);
        free(path);
    }
    fclose(f);
}

static int compareGames(const void* a, const void* b) {
    const DetectedGame* ga = (const DetectedGame*) a;
    const DetectedGame* gb = (const DetectedGame*) b;
    int c = 0;
    const char* x = ga->title; const char* y = gb->title;
    while (*x && *y) {
        c = tolower((unsigned char) *x) - tolower((unsigned char) *y);
        if (c != 0) return c;
        x++; y++;
    }
    c = (int) (unsigned char) *x - (int) (unsigned char) *y;
    if (c != 0) return c;
    return strcmp(ga->path, gb->path);
}

int GameScan_detect(DetectedGame** out, const char* extraRoot) {
    ScanState st;
    memset(&st, 0, sizeof(st));
    st.budget = 60000;

    const char* home = getenv("HOME");
#ifdef _WIN32
    if (home == nullptr) home = getenv("USERPROFILE");
#endif

    if (extraRoot != nullptr && dirExists(extraRoot)) scanDir(&st, extraRoot, 3);

    // The current directory and the one the executable lives in
    scanDir(&st, ".", 2);

#ifdef _WIN32
    const char* pf86 = getenv("ProgramFiles(x86)");
    const char* pf = getenv("ProgramFiles");
    const char* roots[] = { pf86, pf, nullptr };
    for (int i = 0; roots[i]; i++) {
        char* steam = joinPath(roots[i], "Steam");
        scanSteamLibraries(&st, steam);
        char* common = joinPath(steam, "steamapps/common");
        if (dirExists(common)) scanDir(&st, common, 3);
        free(common); free(steam);
        char* gog = joinPath(roots[i], "GOG Galaxy/Games");
        if (dirExists(gog)) scanDir(&st, gog, 3);
        free(gog);
    }
    for (char drive = 'C'; drive <= 'Z'; drive++) {
        char p[64];
        snprintf(p, sizeof(p), "%c:\\SteamLibrary\\steamapps\\common", drive);
        if (dirExists(p)) scanDir(&st, p, 3);
        snprintf(p, sizeof(p), "%c:\\GOG Games", drive);
        if (dirExists(p)) scanDir(&st, p, 3);
        snprintf(p, sizeof(p), "%c:\\Games", drive);
        if (dirExists(p)) scanDir(&st, p, 3);
    }
#else
    if (home != nullptr) {
        static const char* const steamRoots[] = {
            ".steam/steam", ".steam/root", ".local/share/Steam", ".var/app/com.valvesoftware.Steam/.local/share/Steam",
            "Library/Application Support/Steam", "snap/steam/common/.local/share/Steam", nullptr };
        for (int i = 0; steamRoots[i]; i++) {
            char* root = joinPath(home, steamRoots[i]);
            if (dirExists(root)) {
                scanSteamLibraries(&st, root);
                char* common = joinPath(root, "steamapps/common");
                if (dirExists(common)) scanDir(&st, common, 3);
                free(common);
            }
            free(root);
        }
        static const char* const folders[] = { "Games", "games", "Downloads", "Desktop", "Documents", "GOG Games", nullptr };
        for (int i = 0; folders[i]; i++) {
            char* dir = joinPath(home, folders[i]);
            if (dirExists(dir)) scanDir(&st, dir, 3);
            free(dir);
        }
        // Games sitting directly in (or one level below) the home directory
        scanDir(&st, home, 2);
    }
    scanDir(&st, "/Applications", 2);
    scanDir(&st, "/opt", 2);
    scanDir(&st, "/usr/share/games", 2);
#endif

    if (st.count > 1) qsort(st.items, (size_t) st.count, sizeof(DetectedGame), compareGames);
    *out = st.items;
    return st.count;
}

void GameScan_free(DetectedGame* games, int count) {
    for (int i = 0; i < count; i++) {
        free(games[i].path);
        free(games[i].title);
    }
    free(games);
}

int GameScan_findByFingerprint(const DetectedGame* games, int count, uint64_t size, uint64_t hash) {
    for (int i = 0; i < count; i++) {
        if (games[i].size != size) continue;
        NetFileFingerprint fp;
        if (Net_fingerprintFile(games[i].path, &fp) && fp.hash == hash) return i;
    }
    return -1;
}
