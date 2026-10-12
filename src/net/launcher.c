// Graphical launcher: choose a game (auto-detected or typed path), then host a server, join one or play offline.
// Drawn with the renderer's built-in debug font and rectangles, so it works on every desktop backend without a GUI toolkit.

// winsock2.h has to come before windows.h, which other headers pull in
#include "net_link.h"
#include "launcher.h"

#include "game_scan.h"
#include "net_client.h"
#include "net_link.h"
#include "net_proto.h"
#include "net_server.h"
#include "net_util.h"

#include "cli/args.h"
#include "debug_font/debug_font.h"
#include "gettime.h"
#include "log.h"
#include "loop.h"
#include "platformdefs.h"
#include "renderer.h"
#include "runner.h"
#include "runner_keyboard.h"
#include "runner_mouse.h"
#include "stb_ds.h"
#include "utils.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include <shlobj.h>
#else
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

// ===[ Colours (BGR, as the renderer expects) ]===
#define UI_RGB(r, g, b) ((uint32_t) (((b) << 16) | ((g) << 8) | (r)))
#define COL_BG        UI_RGB(24, 26, 33)
#define COL_PANEL     UI_RGB(33, 36, 46)
#define COL_PANEL_HI  UI_RGB(44, 48, 62)
#define COL_BORDER    UI_RGB(64, 70, 90)
#define COL_TEXT      UI_RGB(228, 230, 238)
#define COL_DIM       UI_RGB(140, 146, 165)
#define COL_ACCENT    UI_RGB(245, 180, 66)   // butterscotch
#define COL_ACCENT_HI UI_RGB(255, 205, 110)
#define COL_ACCENT_DK UI_RGB(176, 120, 30)
#define COL_GOOD      UI_RGB(110, 210, 140)
#define COL_BAD       UI_RGB(240, 100, 100)
#define COL_SEL       UI_RGB(58, 52, 36)

#define MAX_PATH_TEXT 1024
#define MAX_GAMES 128

typedef enum { MODE_HOST = 0, MODE_JOIN = 1, MODE_OFFLINE = 2 } LauncherMode;

typedef struct {
    char* path;
    char* title;
    bool manual;
} GameEntry;

typedef struct {
    // Persistent settings
    char name[48];
    char joinAddress[128];
    char hostPort[8];
    char maxPlayers[8];
    int mode;
    char lastGame[MAX_PATH_TEXT];
    char* manualPaths[MAX_GAMES];
    int manualCount;
} Settings;

typedef struct {
    Settings settings;
    GameEntry games[MAX_GAMES];
    int gameCount;
    int selected;       // index into games, -1 = none
    int listScroll;     // first visible row
    char pathInput[MAX_PATH_TEXT];
    char status[512];
    uint32_t statusColor;
} Launcher;

// ===[ Settings file ]===

static void configPath(char* out, size_t cap) {
    out[0] = '\0';
#ifdef _WIN32
    const char* base = getenv("APPDATA");
    if (base != nullptr) snprintf(out, cap, "%s\\butterscotch\\launcher.ini", base);
#elif defined(__APPLE__)
    const char* home = getenv("HOME");
    if (home != nullptr) snprintf(out, cap, "%s/Library/Application Support/butterscotch/launcher.ini", home);
#else
    const char* xdg = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    if (xdg != nullptr && xdg[0] != '\0') snprintf(out, cap, "%s/butterscotch/launcher.ini", xdg);
    else if (home != nullptr) snprintf(out, cap, "%s/.config/butterscotch/launcher.ini", home);
#endif
}

static void makeConfigDir(const char* file) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", file);
    char* slash = strrchr(dir, '/');
#ifdef _WIN32
    char* bs = strrchr(dir, '\\');
    if (bs != nullptr && (slash == nullptr || bs > slash)) slash = bs;
#endif
    if (slash == nullptr) return;
    *slash = '\0';
    // Create each missing level
    for (char* p = dir + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = '\0';
#ifdef _WIN32
            CreateDirectoryA(dir, nullptr);
#else
            mkdir(dir, 0755);
#endif
            *p = c;
        }
    }
#ifdef _WIN32
    CreateDirectoryA(dir, nullptr);
#else
    mkdir(dir, 0755);
#endif
}

static void defaultSettings(Settings* s) {
    memset(s, 0, sizeof(*s));
    const char* user = getenv("USER");
#ifdef _WIN32
    if (user == nullptr) user = getenv("USERNAME");
#endif
    snprintf(s->name, sizeof(s->name), "%s", user != nullptr && user[0] != '\0' ? user : "player");
    snprintf(s->joinAddress, sizeof(s->joinAddress), "127.0.0.1:%d", NET_DEFAULT_PORT);
    snprintf(s->hostPort, sizeof(s->hostPort), "%d", NET_DEFAULT_PORT);
    snprintf(s->maxPlayers, sizeof(s->maxPlayers), "8");
    s->mode = MODE_HOST;
}

static void loadSettings(Settings* s) {
    defaultSettings(s);
    char path[1024];
    configPath(path, sizeof(path));
    if (path[0] == '\0') return;
    FILE* f = fopen(path, "r");
    if (f == nullptr) return;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        char* eq = strchr(line, '=');
        if (eq == nullptr) continue;
        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;
        if (strcmp(key, "name") == 0) snprintf(s->name, sizeof(s->name), "%s", val);
        else if (strcmp(key, "join") == 0) snprintf(s->joinAddress, sizeof(s->joinAddress), "%s", val);
        else if (strcmp(key, "port") == 0) snprintf(s->hostPort, sizeof(s->hostPort), "%s", val);
        else if (strcmp(key, "maxplayers") == 0) snprintf(s->maxPlayers, sizeof(s->maxPlayers), "%s", val);
        else if (strcmp(key, "mode") == 0) s->mode = atoi(val) >= 0 && atoi(val) <= 2 ? atoi(val) : 0;
        else if (strcmp(key, "lastgame") == 0) snprintf(s->lastGame, sizeof(s->lastGame), "%s", val);
        else if (strcmp(key, "game") == 0 && s->manualCount < MAX_GAMES) s->manualPaths[s->manualCount++] = safeStrdup(val);
    }
    fclose(f);
}

static void saveSettings(const Launcher* l) {
    char path[1024];
    configPath(path, sizeof(path));
    if (path[0] == '\0') return;
    makeConfigDir(path);
    FILE* f = fopen(path, "w");
    if (f == nullptr) return;
    const Settings* s = &l->settings;
    fprintf(f, "name=%s\njoin=%s\nport=%s\nmaxplayers=%s\nmode=%d\n", s->name, s->joinAddress, s->hostPort, s->maxPlayers, s->mode);
    if (l->selected >= 0 && l->selected < l->gameCount) fprintf(f, "lastgame=%s\n", l->games[l->selected].path);
    for (int i = 0; i < l->gameCount; i++) {
        if (l->games[i].manual) fprintf(f, "game=%s\n", l->games[i].path);
    }
    fclose(f);
}

// ===[ Games ]===

static void clearGames(Launcher* l) {
    for (int i = 0; i < l->gameCount; i++) {
        free(l->games[i].path);
        free(l->games[i].title);
    }
    l->gameCount = 0;
}

static int findGame(const Launcher* l, const char* path) {
    for (int i = 0; i < l->gameCount; i++) if (strcmp(l->games[i].path, path) == 0) return i;
    return -1;
}

static void setStatus(Launcher* l, uint32_t color, const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vsnprintf(l->status, sizeof(l->status), fmt, va);
    va_end(va);
    l->statusColor = color;
}

static int addGame(Launcher* l, const char* path, const char* title, bool manual) {
    int existing = findGame(l, path);
    if (existing >= 0) return existing;
    if (l->gameCount >= MAX_GAMES) return -1;
    l->games[l->gameCount].path = safeStrdup(path);
    l->games[l->gameCount].title = safeStrdup(title);
    l->games[l->gameCount].manual = manual;
    return l->gameCount++;
}

static void rescan(Launcher* l) {
    // Keep manual entries, replace everything else
    char* keepSelected = (l->selected >= 0 && l->selected < l->gameCount) ? safeStrdup(l->games[l->selected].path) : nullptr;
    char* manualPaths[MAX_GAMES];
    int manualCount = 0;
    for (int i = 0; i < l->gameCount; i++) {
        if (l->games[i].manual) manualPaths[manualCount++] = safeStrdup(l->games[i].path);
    }
    clearGames(l);

    DetectedGame* found = nullptr;
    int n = GameScan_detect(&found, nullptr);
    for (int i = 0; i < n; i++) addGame(l, found[i].path, found[i].title, false);
    GameScan_free(found, n);

    for (int i = 0; i < manualCount; i++) {
        if (findGame(l, manualPaths[i]) < 0) {
            DetectedGame g;
            if (GameScan_describe(manualPaths[i], &g)) {
                addGame(l, g.path, g.title, true);
                free(g.path);
                free(g.title);
            }
        } else {
            l->games[findGame(l, manualPaths[i])].manual = true;
        }
        free(manualPaths[i]);
    }
    l->selected = keepSelected != nullptr ? findGame(l, keepSelected) : -1;
    free(keepSelected);
    setStatus(l, COL_DIM, "Found %d game%s.", l->gameCount, l->gameCount == 1 ? "" : "s");
}

// Strips quotes / whitespace people leave when pasting paths
static void cleanPath(char* s) {
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char) s[n - 1])) s[--n] = '\0';
    char* start = s;
    while (*start && isspace((unsigned char) *start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

static bool isDirectory(const char* path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat sb;
    return stat(path, &sb) == 0 && S_ISDIR(sb.st_mode);
#endif
}

// Adds what the user typed: a data file, or a folder containing one (searched a few levels deep)
static void addManualPath(Launcher* l) {
    cleanPath(l->pathInput);
    if (l->pathInput[0] == '\0') return;
#ifndef _WIN32
    if (l->pathInput[0] == '~' && (l->pathInput[1] == '/' || l->pathInput[1] == '\0')) {
        const char* home = getenv("HOME");
        if (home != nullptr) {
            char tmp[MAX_PATH_TEXT];
            snprintf(tmp, sizeof(tmp), "%s%s", home, l->pathInput + 1);
            snprintf(l->pathInput, sizeof(l->pathInput), "%s", tmp);
        }
    }
#endif
    if (isDirectory(l->pathInput)) {
        DetectedGame* found = nullptr;
        int n = GameScan_detect(&found, l->pathInput);
        int added = 0, firstIdx = -1;
        for (int i = 0; i < n; i++) {
            // GameScan_detect also returns the usual places; only keep what lives under the typed folder
            if (strncmp(found[i].path, l->pathInput, strlen(l->pathInput)) != 0 &&
                strncmp(found[i].path, "./", 2) != 0) continue;
            int before = l->gameCount;
            int idx = addGame(l, found[i].path, found[i].title, true);
            if (idx >= 0) {
                if (l->games[idx].manual == false) l->games[idx].manual = true;
                if (firstIdx < 0) firstIdx = idx;
                if (l->gameCount > before) added++;
            }
        }
        GameScan_free(found, n);
        if (firstIdx >= 0) {
            l->selected = firstIdx;
            setStatus(l, COL_GOOD, "Added %d game(s) from that folder.", added);
            l->pathInput[0] = '\0';
        } else {
            setStatus(l, COL_BAD, "No data.win / game.unx / game.ios found in that folder.");
        }
        return;
    }
    DetectedGame g;
    if (!GameScan_describe(l->pathInput, &g)) {
        setStatus(l, COL_BAD, "'%s' is not a readable GameMaker data file.", l->pathInput);
        return;
    }
    int idx = addGame(l, g.path, g.title, true);
    if (idx >= 0) {
        l->games[idx].manual = true;
        l->selected = idx;
        setStatus(l, COL_GOOD, "Added %s.", g.title);
        l->pathInput[0] = '\0';
    }
    free(g.path);
    free(g.title);
}

// ===[ Immediate-mode UI ]===

typedef struct {
    Renderer* renderer;
    Runner* runner;
    int32_t w, h;       // framebuffer size in pixels
    float s;            // UI scale
    double mx, my;
    bool clicked, down;
    bool pressed[GML_KEY_COUNT];
    uint32_t chars[32];
    int charCount;
    int wheel;
    int focus;          // id of the focused text field (0 = none)
    int caret;
    int holdCode;
    uint64_t holdSince, holdLast;
    uint64_t now;
} Ui;

static float textWidth(const char* text, float scale) {
    float w = 0.0f;
    for (const unsigned char* p = (const unsigned char*) text; *p; p++) {
        if (*p < DEBUGFONT_FIRST_CP || *p > DEBUGFONT_LAST_CP) { w += 24.0f * scale; continue; }
        w += (float) debugFontGlyphs[*p - DEBUGFONT_FIRST_CP].xadvance * scale;
    }
    return w;
}

static void drawRect(Ui* ui, float x, float y, float w, float h, uint32_t color) {
    ui->renderer->vtable->drawRectangle(ui->renderer, x, y, x + w, y + h, color, 1.0f, false);
}

static void drawOutline(Ui* ui, float x, float y, float w, float h, uint32_t color) {
    float t = ui->s;
    drawRect(ui, x, y, w, t, color);
    drawRect(ui, x, y + h - t, w, t, color);
    drawRect(ui, x, y, t, h, color);
    drawRect(ui, x + w - t, y, t, h, color);
}

static float textScale(Ui* ui, float em) { return em * ui->s / (float) DEBUGFONT_LINE_HEIGHT; } // em = line height in px at UI scale 1

static void drawText(Ui* ui, const char* text, float x, float y, float em, uint32_t color) {
    float sc = textScale(ui, em);
    ui->renderer->vtable->drawTextUI(ui->renderer, text, x, y, sc, sc, 0.0f, (int32_t) color, (int32_t) color, (int32_t) color, (int32_t) color, 1.0f, -1.0f);
}

// Copies text that fits into maxW (adds "..." when cut)
static void fitText(char* out, size_t cap, const char* text, float maxW, float scale) {
    snprintf(out, cap, "%s", text);
    if (textWidth(out, scale) <= maxW) return;
    size_t n = strlen(out);
    while (n > 3 && textWidth(out, scale) + textWidth("...", scale) > maxW) out[--n] = '\0';
    if (n + 3 < cap) strcat(out, "...");
}

// Same, but keeps the END of the text visible (used for the tail of long paths)
static void fitTail(char* out, size_t cap, const char* text, float maxW, float scale) {
    size_t n = strlen(text);
    size_t start = 0;
    while (start < n && textWidth(text + start, scale) > maxW) start++;
    if (start == 0) { snprintf(out, cap, "%s", text); return; }
    snprintf(out, cap, "...%s", text + start + (start + 3 < n ? 3 : 0));
}

static bool inside(Ui* ui, float x, float y, float w, float h) {
    return ui->mx >= x && ui->mx < x + w && ui->my >= y && ui->my < y + h;
}

static bool uiButton(Ui* ui, float x, float y, float w, float h, const char* label, bool enabled, bool primary) {
    bool hover = enabled && inside(ui, x, y, w, h);
    uint32_t bg = !enabled ? COL_PANEL : primary ? (hover ? COL_ACCENT_HI : COL_ACCENT) : (hover ? COL_PANEL_HI : COL_PANEL);
    drawRect(ui, x, y, w, h, bg);
    drawOutline(ui, x, y, w, h, primary && enabled ? COL_ACCENT_DK : COL_BORDER);
    float em = 15.0f;
    float sc = textScale(ui, em);
    float tw = textWidth(label, sc);
    uint32_t fg = !enabled ? COL_DIM : primary ? UI_RGB(40, 28, 8) : COL_TEXT;
    drawText(ui, label, x + (w - tw) / 2, y + (h - em * ui->s) / 2, em, fg);
    return hover && ui->clicked;
}

static void insertCodepoint(char* buf, size_t cap, int* caret, uint32_t cp) {
    char enc[5];
    int n = 0;
    if (cp < 0x80) enc[n++] = (char) cp;
    else if (cp < 0x800) { enc[n++] = (char) (0xC0 | (cp >> 6)); enc[n++] = (char) (0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { enc[n++] = (char) (0xE0 | (cp >> 12)); enc[n++] = (char) (0x80 | ((cp >> 6) & 0x3F)); enc[n++] = (char) (0x80 | (cp & 0x3F)); }
    else { enc[n++] = (char) (0xF0 | (cp >> 18)); enc[n++] = (char) (0x80 | ((cp >> 12) & 0x3F)); enc[n++] = (char) (0x80 | ((cp >> 6) & 0x3F)); enc[n++] = (char) (0x80 | (cp & 0x3F)); }
    size_t len = strlen(buf);
    if (len + (size_t) n + 1 > cap) return;
    memmove(buf + *caret + n, buf + *caret, len - (size_t) *caret + 1);
    memcpy(buf + *caret, enc, (size_t) n);
    *caret += n;
}

static int prevCharStart(const char* buf, int caret) {
    int i = caret - 1;
    while (i > 0 && ((unsigned char) buf[i] & 0xC0) == 0x80) i--;
    return i < 0 ? 0 : i;
}

// Returns true when Enter was pressed inside the field
static bool uiTextField(Ui* ui, int id, float x, float y, float w, float h, char* buf, size_t cap, const char* placeholder, bool digitsOnly) {
    bool hover = inside(ui, x, y, w, h);
    bool focused = ui->focus == id;
    bool submitted = false;
    float em = 15.0f;
    float sc = textScale(ui, em);
    float pad = 8.0f * ui->s;

    if (ui->clicked) {
        if (hover) {
            if (!focused) { ui->focus = id; ui->caret = (int) strlen(buf); focused = true; }
            else {
                // Place the caret where the user clicked
                float rel = (float) ui->mx - (x + pad);
                int pos = 0;
                char tmp[MAX_PATH_TEXT];
                size_t len = strlen(buf);
                for (size_t i = 0; i <= len; i++) {
                    if (i < len && ((unsigned char) buf[i] & 0xC0) == 0x80) continue;
                    snprintf(tmp, sizeof(tmp), "%.*s", (int) i, buf);
                    if (textWidth(tmp, sc) <= rel) pos = (int) i;
                }
                ui->caret = pos;
            }
        } else if (focused) {
            ui->focus = 0;
            focused = false;
        }
    }

    if (focused) {
        size_t len = strlen(buf);
        if (ui->caret > (int) len) ui->caret = (int) len;
        for (int i = 0; i < ui->charCount; i++) {
            uint32_t cp = ui->chars[i];
            if (cp < 32 || cp == 127) continue;
            if (digitsOnly && !(cp >= '0' && cp <= '9')) continue;
            insertCodepoint(buf, cap, &ui->caret, cp);
        }
        len = strlen(buf);
        bool repeatBack = ui->holdCode == VK_BACKSPACE && ui->holdLast == ui->now;
        bool repeatDel = ui->holdCode == VK_DELETE && ui->holdLast == ui->now;
        bool repeatLeft = ui->holdCode == VK_LEFT && ui->holdLast == ui->now;
        bool repeatRight = ui->holdCode == VK_RIGHT && ui->holdLast == ui->now;
        if ((ui->pressed[VK_BACKSPACE] || repeatBack) && ui->caret > 0) {
            int start = prevCharStart(buf, ui->caret);
            memmove(buf + start, buf + ui->caret, len - (size_t) ui->caret + 1);
            ui->caret = start;
        }
        if ((ui->pressed[VK_DELETE] || repeatDel) && ui->caret < (int) len) {
            int end = ui->caret + 1;
            while (end < (int) len && ((unsigned char) buf[end] & 0xC0) == 0x80) end++;
            memmove(buf + ui->caret, buf + end, len - (size_t) end + 1);
        }
        if ((ui->pressed[VK_LEFT] || repeatLeft) && ui->caret > 0) ui->caret = prevCharStart(buf, ui->caret);
        if ((ui->pressed[VK_RIGHT] || repeatRight) && ui->caret < (int) len) {
            ui->caret++;
            while (ui->caret < (int) len && ((unsigned char) buf[ui->caret] & 0xC0) == 0x80) ui->caret++;
        }
        if (ui->pressed[VK_HOME]) ui->caret = 0;
        if (ui->pressed[VK_END]) ui->caret = (int) strlen(buf);
        if (ui->pressed[VK_ENTER]) submitted = true;
    }

    drawRect(ui, x, y, w, h, COL_BG);
    drawOutline(ui, x, y, w, h, focused ? COL_ACCENT : (hover ? COL_DIM : COL_BORDER));

    // Scroll so the caret stays visible
    char shown[MAX_PATH_TEXT];
    float innerW = w - 2 * pad;
    const char* text = buf;
    int caret = focused ? ui->caret : (int) strlen(buf);
    size_t skip = 0;
    {
        char tmp[MAX_PATH_TEXT];
        snprintf(tmp, sizeof(tmp), "%.*s", caret, buf);
        while (tmp[skip] != '\0' && textWidth(tmp + skip, sc) > innerW - 4 * ui->s) skip++;
        while (tmp[skip] != '\0' && ((unsigned char) tmp[skip] & 0xC0) == 0x80) skip++;
    }
    snprintf(shown, sizeof(shown), "%s", text + skip);
    // Trim the right side to fit
    while (*shown && textWidth(shown, sc) > innerW) shown[strlen(shown) - 1] = '\0';

    float ty = y + (h - em * ui->s) / 2;
    if (buf[0] == '\0' && !focused) {
        drawText(ui, placeholder, x + pad, ty, em, COL_DIM);
    } else {
        drawText(ui, shown, x + pad, ty, em, COL_TEXT);
    }
    if (focused && ((ui->now / 500000000ull) & 1) == 0) {
        char tmp[MAX_PATH_TEXT];
        snprintf(tmp, sizeof(tmp), "%.*s", caret - (int) skip > 0 ? caret - (int) skip : 0, buf + skip);
        float cx = x + pad + textWidth(tmp, sc);
        drawRect(ui, cx, y + 5 * ui->s, 1.5f * ui->s, h - 10 * ui->s, COL_ACCENT);
    }
    return submitted;
}

// ===[ Launching ]===

#ifdef _WIN32
typedef HANDLE ChildProcess;
#define NO_CHILD INVALID_HANDLE_VALUE
#else
typedef pid_t ChildProcess;
#define NO_CHILD ((pid_t) -1)
#endif

static void selfExePath(const char* argv0, char* out, size_t cap) {
    snprintf(out, cap, "%s", argv0);
#ifdef _WIN32
    GetModuleFileNameA(nullptr, out, (DWORD) cap);
#elif defined(__APPLE__)
    uint32_t size = (uint32_t) cap;
    _NSGetExecutablePath(out, &size);
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n > 0) out[n] = '\0';
#endif
}

static ChildProcess spawnProcess(const char* exe, char* const* argv) {
#ifdef _WIN32
    char cmd[4096];
    size_t pos = 0;
    for (int i = 0; argv[i] != nullptr; i++) {
        // Quotes in a user-typed value would let it smuggle extra arguments into the child's command line
        for (const char* q = argv[i]; *q; q++) if (*q == '"') return NO_CHILD;
        if (pos >= sizeof(cmd)) return NO_CHILD;
        pos += (size_t) snprintf(cmd + pos, sizeof(cmd) - pos, "%s\"%s\"", i ? " " : "", argv[i]);
    }
    if (pos >= sizeof(cmd)) return NO_CHILD;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(exe, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return NO_CHILD;
    CloseHandle(pi.hThread);
    return pi.hProcess;
#else
    pid_t pid = fork();
    if (pid == 0) {
        execv(exe, argv);
        _exit(127);
    }
    return pid;
#endif
}

static void stopProcess(ChildProcess p) {
    if (p == NO_CHILD) return;
#ifdef _WIN32
    TerminateProcess(p, 0);
    WaitForSingleObject(p, 3000);
    CloseHandle(p);
#else
    kill(p, SIGTERM);
    for (int i = 0; i < 50; i++) {
        int status;
        if (waitpid(p, &status, WNOHANG) != 0) return;
        usleep(100000);
    }
    kill(p, SIGKILL);
    waitpid(p, nullptr, 0);
#endif
}

static bool processAlive(ChildProcess p) {
#ifdef _WIN32
    return WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
#else
    int status;
    return waitpid(p, &status, WNOHANG) == 0;
#endif
}

typedef struct {
    int action; // 0 = quit, 1 = host, 2 = join, 3 = offline
    char gamePath[MAX_PATH_TEXT];
    char name[48];
    char address[128];
    char port[8];
    char maxPlayers[8];
} LaunchRequest;

// Runs a command line through the normal CLI parser so the launcher and the flags behave identically
static void runWithArgs(const char* argv0, int argc, char** argv, int* outExit, int mode) {
    (void) argv0;
    CommandLineArgs args;
    parseCommandLineArgs(&args, argc, argv);
    int ret;
    if (mode == NET_MODE_SERVER) ret = NetServer_run(args, argv[0]);
    else if (mode == NET_MODE_CLIENT) ret = NetClient_run(args, argv[0]);
    else ret = loop(args, argv[0]);
    freeCommandLineArgs(&args);
    *outExit = ret;
}

static void performLaunch(const char* argv0, const LaunchRequest* req, char* messageOut, size_t messageCap) {
    messageOut[0] = '\0';
    char exe[1024];
    selfExePath(argv0, exe, sizeof(exe));
    int exitCode = 0;

    if (req->action == 3) {
        char* argv[] = { exe, (char*) req->gamePath, nullptr };
        runWithArgs(exe, 2, argv, &exitCode, NET_MODE_STANDALONE);
        return;
    }

    if (req->action == 2) {
        char* argv[10];
        int n = 0;
        argv[n++] = exe;
        argv[n++] = (char*) "--connect";
        argv[n++] = (char*) req->address;
        argv[n++] = (char*) "--name";
        argv[n++] = (char*) req->name;
        if (req->gamePath[0] != '\0') argv[n++] = (char*) req->gamePath;
        argv[n] = nullptr;
        runWithArgs(exe, n, argv, &exitCode, NET_MODE_CLIENT);
        if (exitCode != 0) snprintf(messageOut, messageCap, "%s", NetClient_lastMessage()[0] ? NetClient_lastMessage() : "Disconnected from the server.");
        return;
    }

    // Host: start the server as a child process (the runner keeps global state, so one process can't be both) and join it
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%s", req->port[0] ? req->port : "7777");
    char* sargv[16];
    int n = 0;
    sargv[n++] = exe;
    sargv[n++] = (char*) "--server";
    sargv[n++] = (char*) "--port";
    sargv[n++] = portStr;
    sargv[n++] = (char*) "--max-clients";
    sargv[n++] = (char*) (req->maxPlayers[0] ? req->maxPlayers : "8");
    sargv[n++] = (char*) req->gamePath;
    sargv[n] = nullptr;
    ChildProcess server = spawnProcess(exe, sargv);
    if (server == NO_CHILD) {
        snprintf(messageOut, messageCap, "Could not start the server process.");
        return;
    }
    char address[64];
    snprintf(address, sizeof(address), "127.0.0.1:%s", portStr);
    char* cargv[10];
    n = 0;
    cargv[n++] = exe;
    cargv[n++] = (char*) "--connect";
    cargv[n++] = address;
    cargv[n++] = (char*) "--name";
    cargv[n++] = (char*) req->name;
    cargv[n++] = (char*) req->gamePath;
    cargv[n] = nullptr;
    runWithArgs(exe, n, cargv, &exitCode, NET_MODE_CLIENT);
    if (exitCode != 0) {
        if (!processAlive(server)) snprintf(messageOut, messageCap, "The server stopped unexpectedly (is port %s already in use?).", portStr);
        else if (NetClient_lastMessage()[0]) snprintf(messageOut, messageCap, "%s", NetClient_lastMessage());
    }
    stopProcess(server);
}

// ===[ Main screen ]===

static void drawLauncher(Ui* ui, Launcher* l, LaunchRequest* req) {
    float s = ui->s;
    float W = (float) ui->w, H = (float) ui->h;
    float margin = 18 * s;

    drawRect(ui, 0, 0, W, H, COL_BG);

    // Header
    drawText(ui, "Butterscotch Slices", margin, margin - 4 * s, 30, COL_ACCENT);
    drawText(ui, "Multiplayer fork of Butterscotch: one machine runs the game, all players join in",
             margin + 340 * s, margin + 6 * s, 13, COL_DIM);

    float top = margin + 44 * s;
    float bottom = H - margin - 30 * s;
    float leftW = (W - 3 * margin) * 0.52f;
    float rightX = margin * 2 + leftW;
    float rightW = W - margin - rightX;

    // ---- Left: game list ----
    drawRect(ui, margin, top, leftW, bottom - top, COL_PANEL);
    drawOutline(ui, margin, top, leftW, bottom - top, COL_BORDER);
    drawText(ui, "GAMES", margin + 12 * s, top + 10 * s, 14, COL_DIM);
    float rescanW = 92 * s;
    if (uiButton(ui, margin + leftW - rescanW - 10 * s, top + 6 * s, rescanW, 28 * s, "Rescan", true, false)) {
        rescan(l);
    }

    float rowH = 40 * s;
    float listTop = top + 42 * s;
    float pathAreaH = 92 * s;
    float listBottom = bottom - pathAreaH;
    int rows = (int) ((listBottom - listTop) / rowH);
    if (rows < 1) rows = 1;
    if (ui->wheel != 0 && inside(ui, margin, listTop, leftW, listBottom - listTop)) l->listScroll -= ui->wheel;
    if (l->listScroll > l->gameCount - rows) l->listScroll = l->gameCount - rows;
    if (l->listScroll < 0) l->listScroll = 0;

    if (l->gameCount == 0) {
        drawText(ui, "No games found automatically.", margin + 14 * s, listTop + 12 * s, 15, COL_TEXT);
        drawText(ui, "Type the path of a data.win (or the game folder)", margin + 14 * s, listTop + 38 * s, 13, COL_DIM);
        drawText(ui, "below and press Add.", margin + 14 * s, listTop + 58 * s, 13, COL_DIM);
    }
    for (int r = 0; r < rows; r++) {
        int idx = l->listScroll + r;
        if (idx >= l->gameCount) break;
        float ry = listTop + r * rowH;
        bool hover = inside(ui, margin + 4 * s, ry, leftW - 8 * s, rowH);
        bool sel = idx == l->selected;
        if (sel) drawRect(ui, margin + 4 * s, ry, leftW - 8 * s, rowH - 2 * s, COL_SEL);
        else if (hover) drawRect(ui, margin + 4 * s, ry, leftW - 8 * s, rowH - 2 * s, COL_PANEL_HI);
        if (sel) drawRect(ui, margin + 4 * s, ry, 4 * s, rowH - 2 * s, COL_ACCENT);
        char buf[512];
        float tsc = textScale(ui, 16);
        fitText(buf, sizeof(buf), l->games[idx].title, leftW - 40 * s, tsc);
        drawText(ui, buf, margin + 16 * s, ry + 3 * s, 16, sel ? COL_ACCENT_HI : COL_TEXT);
        float psc = textScale(ui, 11);
        fitTail(buf, sizeof(buf), l->games[idx].path, leftW - 40 * s, psc);
        drawText(ui, buf, margin + 16 * s, ry + 21 * s, 11, COL_DIM);
        if (hover && ui->clicked) {
            l->selected = (idx == l->selected) ? -1 : idx;
        }
    }
    // Scroll bar
    if (l->gameCount > rows) {
        float trackH = listBottom - listTop;
        float barH = trackH * rows / l->gameCount;
        float barY = listTop + trackH * l->listScroll / l->gameCount;
        drawRect(ui, margin + leftW - 8 * s, barY, 4 * s, barH, COL_BORDER);
    }

    // Manual path entry
    float py = listBottom + 8 * s;
    drawText(ui, "Add a game by path (data.win, game.unx, game.ios or the game folder):", margin + 12 * s, py, 12, COL_DIM);
    float addW = 70 * s;
    bool submitted = uiTextField(ui, 1, margin + 12 * s, py + 22 * s, leftW - 24 * s - addW - 8 * s, 34 * s, l->pathInput, sizeof(l->pathInput), "/path/to/UNDERTALE/data.win", false);
    bool addClicked = uiButton(ui, margin + leftW - 12 * s - addW, py + 22 * s, addW, 34 * s, "Add", l->pathInput[0] != '\0', false);
    if (submitted || addClicked) addManualPath(l);

    // ---- Right: mode tabs + settings ----
    drawRect(ui, rightX, top, rightW, bottom - top, COL_PANEL);
    drawOutline(ui, rightX, top, rightW, bottom - top, COL_BORDER);
    static const char* const tabs[3] = { "Host", "Join", "Offline" };
    float tabW = (rightW - 24 * s) / 3;
    for (int i = 0; i < 3; i++) {
        float tx = rightX + 12 * s + i * tabW;
        bool active = l->settings.mode == i;
        bool hover = inside(ui, tx, top + 10 * s, tabW - 4 * s, 34 * s);
        drawRect(ui, tx, top + 10 * s, tabW - 4 * s, 34 * s, active ? COL_ACCENT : (hover ? COL_PANEL_HI : COL_BG));
        drawOutline(ui, tx, top + 10 * s, tabW - 4 * s, 34 * s, active ? COL_ACCENT_DK : COL_BORDER);
        float sc = textScale(ui, 16);
        float tw = textWidth(tabs[i], sc);
        drawText(ui, tabs[i], tx + (tabW - 4 * s - tw) / 2, top + 16 * s, 16, active ? UI_RGB(40, 28, 8) : COL_TEXT);
        if (hover && ui->clicked) { l->settings.mode = i; ui->focus = 0; }
    }

    float px = rightX + 16 * s;
    float pw = rightW - 32 * s;
    float y = top + 62 * s;
    const GameEntry* game = (l->selected >= 0 && l->selected < l->gameCount) ? &l->games[l->selected] : nullptr;
    bool canStart = false;
    const char* startLabel = "Start";

    drawText(ui, "Your name", px, y, 13, COL_DIM);
    uiTextField(ui, 2, px, y + 20 * s, pw, 34 * s, l->settings.name, sizeof(l->settings.name), "player", false);
    y += 66 * s;

    if (l->settings.mode == MODE_HOST) {
        startLabel = "Start hosting";
        drawText(ui, "Port", px, y, 13, COL_DIM);
        drawText(ui, "Max players", px + pw * 0.5f, y, 13, COL_DIM);
        uiTextField(ui, 3, px, y + 20 * s, pw * 0.45f, 34 * s, l->settings.hostPort, sizeof(l->settings.hostPort), "7777", true);
        uiTextField(ui, 4, px + pw * 0.5f, y + 20 * s, pw * 0.45f, 34 * s, l->settings.maxPlayers, sizeof(l->settings.maxPlayers), "8", true);
        y += 72 * s;
        canStart = game != nullptr && l->settings.hostPort[0] != '\0';
        drawText(ui, "The game runs on this computer. Players connect to", px, y, 12, COL_DIM);
        drawText(ui, "<your address>:<port>. You play too, as a client.", px, y + 18 * s, 12, COL_DIM);
        y += 48 * s;
        drawText(ui, game != nullptr ? "Game:" : "Select a game on the left.", px, y, 13, game ? COL_DIM : COL_BAD);
        if (game != nullptr) {
            char buf[256];
            fitText(buf, sizeof(buf), game->title, pw - 60 * s, textScale(ui, 15));
            drawText(ui, buf, px + 54 * s, y - 1 * s, 15, COL_ACCENT_HI);
        }
    } else if (l->settings.mode == MODE_JOIN) {
        startLabel = "Connect";
        drawText(ui, "Server address (host or host:port)", px, y, 13, COL_DIM);
        uiTextField(ui, 5, px, y + 20 * s, pw, 34 * s, l->settings.joinAddress, sizeof(l->settings.joinAddress), "192.168.1.20:7777", false);
        y += 72 * s;
        canStart = l->settings.joinAddress[0] != '\0';
        drawText(ui, "You need your own copy of the same game files:", px, y, 12, COL_DIM);
        drawText(ui, "the server only sends draw calls, not graphics or sound.", px, y + 18 * s, 12, COL_DIM);
        y += 48 * s;
        if (game != nullptr) {
            drawText(ui, "Using:", px, y, 13, COL_DIM);
            char buf[256];
            fitText(buf, sizeof(buf), game->title, pw - 60 * s, textScale(ui, 15));
            drawText(ui, buf, px + 54 * s, y - 1 * s, 15, COL_ACCENT_HI);
        } else {
            drawText(ui, "Game files: detected automatically from the", px, y, 13, COL_GOOD);
            drawText(ui, "server's game (select one to force a copy).", px, y + 18 * s, 13, COL_GOOD);
        }
    } else {
        startLabel = "Play offline";
        canStart = game != nullptr;
        drawText(ui, "Plays the selected game on this computer only,", px, y, 12, COL_DIM);
        drawText(ui, "like the regular single-player runner.", px, y + 18 * s, 12, COL_DIM);
        y += 48 * s;
        drawText(ui, game != nullptr ? "Game:" : "Select a game on the left.", px, y, 13, game ? COL_DIM : COL_BAD);
        if (game != nullptr) {
            char buf[256];
            fitText(buf, sizeof(buf), game->title, pw - 60 * s, textScale(ui, 15));
            drawText(ui, buf, px + 54 * s, y - 1 * s, 15, COL_ACCENT_HI);
        }
    }

    float btnH = 46 * s;
    float btnY = bottom - btnH - 16 * s;
    bool go = uiButton(ui, px, btnY, pw, btnH, startLabel, canStart, true);
    if (go && canStart) {
        memset(req, 0, sizeof(*req));
        snprintf(req->name, sizeof(req->name), "%s", l->settings.name[0] ? l->settings.name : "player");
        snprintf(req->address, sizeof(req->address), "%s", l->settings.joinAddress);
        snprintf(req->port, sizeof(req->port), "%s", l->settings.hostPort);
        snprintf(req->maxPlayers, sizeof(req->maxPlayers), "%s", l->settings.maxPlayers);
        if (game != nullptr) snprintf(req->gamePath, sizeof(req->gamePath), "%s", game->path);
        req->action = l->settings.mode == MODE_HOST ? 1 : l->settings.mode == MODE_JOIN ? 2 : 3;
    }

    // ---- Status line ----
    char status[512];
    fitText(status, sizeof(status), l->status, W - 2 * margin, textScale(ui, 13));
    drawText(ui, status, margin, H - margin - 18 * s, 13, l->statusColor);
}

static bool pollUi(Ui* ui) {
    Runner* runner = ui->runner;
    RunnerKeyboard_beginFrame(runner->keyboard);
    RunnerMouse_beginFrame(runner->mouse);
    if (platformHandleEvents()) return false;
    platformGetWindowSize(&ui->w, &ui->h);
    float sc = (float) ui->h / 600.0f;
    ui->s = sc < 1.0f ? 1.0f : (sc > 3.0f ? 3.0f : sc);
    platformGetMousePos(&ui->mx, &ui->my);
    ui->clicked = runner->mouse->buttonPressed[0];
    ui->down = runner->mouse->buttonDown[0];
    ui->wheel = runner->mouse->wheelUp ? 1 : (runner->mouse->wheelDown ? -1 : 0);
    memcpy(ui->pressed, runner->keyboard->keyPressed, sizeof(ui->pressed));
    ui->charCount = runner->keyboard->charQueueLen;
    for (int i = 0; i < ui->charCount; i++) ui->chars[i] = runner->keyboard->charQueue[i];
    ui->now = nowNanos();

    // Key repeat for editing keys (the platform layer only reports the first press)
    static const int repeatKeys[] = { VK_BACKSPACE, VK_DELETE, VK_LEFT, VK_RIGHT };
    bool anyHeld = false;
    for (size_t i = 0; i < sizeof(repeatKeys) / sizeof(repeatKeys[0]); i++) {
        int k = repeatKeys[i];
        if (runner->keyboard->keyDown[k]) {
            anyHeld = true;
            if (ui->holdCode != k) { ui->holdCode = k; ui->holdSince = ui->now; ui->holdLast = 0; }
            if (ui->now - ui->holdSince > 400000000ull && ui->now - ui->holdLast > 35000000ull) {
                ui->holdLast = ui->now;
            }
            break;
        }
    }
    if (!anyHeld) ui->holdCode = 0;
    return true;
}

static int launcherMainImpl(const char* argv0) {
    if (!Net_initSockets()) return 1;
    Launcher* l = (Launcher*) safeCalloc(1, sizeof(Launcher));
    loadSettings(&l->settings);
    l->selected = -1;
    l->statusColor = COL_DIM;
    snprintf(l->status, sizeof(l->status), "Scanning for games...");

    char message[512] = "";
    bool firstRun = true;

    while (true) {
        // ---- window + renderer ----
        CommandLineArgs rargs;
        memset(&rargs, 0, sizeof(rargs));
        rargs.renderer = MODERN_GL;
#if !defined(ENABLE_MODERN_GL) && defined(ENABLE_LEGACY_GL)
        rargs.renderer = LEGACY_GL;
#endif
        gfx = rargs.renderer;
        if (!platformInit(980, 620, "Butterscotch Slices - Launcher", false)) {
            logError("Could not open the launcher window\n");
            return 1;
        }
        if (!loopInitGraphicsContext()) { platformExit(); return 1; }
        Renderer* renderer = loopCreateRenderer(&rargs);
        if (renderer == nullptr) { platformExit(); return 1; }
        DataWin* dummy = (DataWin*) safeCalloc(1, sizeof(DataWin));
        Runner* runner = (Runner*) safeCalloc(1, sizeof(Runner));
        runner->dataWin = dummy;
        runner->renderer = renderer;
        runner->keyboard = RunnerKeyboard_create();
        runner->mouse = RunnerMouse_create();
        runner->gamepads = RunnerGamepad_create();
        runner->applicationSurfaceId = APPLICATION_SURFACE_ID;
        for (int i = 0; i < MAX_SURFACES; i++) runner->surfaceStack[i] = -1;
        renderer->runner = runner;
        renderer->vtable->init(renderer, dummy);
        platformInitFunctions(runner);

        Ui ui;
        memset(&ui, 0, sizeof(ui));
        ui.renderer = renderer;
        ui.runner = runner;
        ui.focus = 0;

        LaunchRequest req;
        memset(&req, 0, sizeof(req));
        bool quit = false;
        int frames = 0;

        if (message[0] != '\0') setStatus(l, COL_BAD, "%s", message);
        message[0] = '\0';

        while (!quit && req.action == 0) {
            if (!pollUi(&ui)) { quit = true; break; }

            // First frame: show the "scanning" text, then scan once
            if (firstRun && frames == 1) {
                rescan(l);
                // Preselect the last used game
                if (l->settings.lastGame[0] != '\0') l->selected = findGame(l, l->settings.lastGame);
                for (int i = 0; i < l->settings.manualCount; i++) {
                    if (findGame(l, l->settings.manualPaths[i]) < 0) {
                        DetectedGame g;
                        if (GameScan_describe(l->settings.manualPaths[i], &g)) {
                            int idx = addGame(l, g.path, g.title, true);
                            if (idx >= 0) l->games[idx].manual = true;
                            free(g.path);
                            free(g.title);
                        }
                    } else {
                        l->games[findGame(l, l->settings.manualPaths[i])].manual = true;
                    }
                    free(l->settings.manualPaths[i]);
                    l->settings.manualPaths[i] = nullptr;
                }
                l->settings.manualCount = 0;
                if (l->settings.lastGame[0] != '\0') l->selected = findGame(l, l->settings.lastGame);
                if (l->selected < 0 && l->gameCount > 0) l->selected = 0;
                firstRun = false;
            }

            // Tab cycles text fields; Escape drops focus
            if (ui.pressed[VK_TAB]) ui.focus = ui.focus >= 5 ? 1 : ui.focus + 1;
            if (ui.pressed[VK_ESCAPE]) ui.focus = 0;

            renderer->vtable->beginGUI(renderer, ui.w, ui.h, 0, 0, ui.w, ui.h, RENDER_TARGET_HOST_FRAMEBUFFER);
            renderer->drawHalign = 0;
            renderer->drawValign = 0;
            drawLauncher(&ui, l, &req);
            renderer->vtable->flush(renderer);
            renderer->vtable->endGUI(renderer);
            if (ui.pressed[VK_F5]) {
                // Hidden helper: save what the launcher currently shows
                static int shotCounter = 0;
                const char* pattern = getenv("BUTTERSCOTCH_LAUNCHER_SHOT");
                loopCaptureScreenshot(pattern != nullptr ? pattern : "launcher-%d.png", shotCounter++, ui.w, ui.h);
            }
            platformSwapBuffers();
            frames++;
            platformSleepUntil(nowNanos() + 8000000ull);
        }

        // ---- tear the launcher window down ----
        renderer->vtable->destroy(renderer);
        RunnerKeyboard_free(runner->keyboard);
        RunnerMouse_free(runner->mouse);
        RunnerGamepad_free(runner->gamepads);
        free(runner);
        free(dummy);
        platformExit();

        saveSettings(l);
        if (quit || req.action == 0) break;

        // ---- run the chosen action; afterwards come back to the launcher ----
        performLaunch(argv0, &req, message, sizeof(message));
    }

    clearGames(l);
    for (int i = 0; i < l->settings.manualCount; i++) free(l->settings.manualPaths[i]);
    free(l);
    return 0;
}

int launcherMain(const char* argv0) {
#ifdef _WIN32
    timeBeginPeriod(1); // 1 ms timer resolution, like the standalone runner (needed for accurate sleeps)
#endif
    int ret = launcherMainImpl(argv0);
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    return ret;
}
