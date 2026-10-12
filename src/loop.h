#ifndef _BS_LOOP_H_
#define _BS_LOOP_H_

#include "platformdefs.h"

char** extractRunnerArguments(char* rawArguments);
// Path of the data.win a game_change() call switches to, relative to the current one (caller frees)
char* loopBuildGameChangeTargetPath(const char* currentDataWinPath, const char* workingDirectory, const char* dataWinFilename);
void loopResolveWindowSize(const CommandLineArgs* args, uint32_t gen8Width, uint32_t gen8Height, int32_t* outW, int32_t* outH);

// Shared with the network client: window / renderer set-up (gfx must be set before)
bool loopInitGraphicsContext(void);
Renderer* loopCreateRenderer(const CommandLineArgs* args);
void loopPreloadTextures(Renderer* renderer, DataWin* dataWin, const CommandLineArgs* args);
void loopClearHostFramebuffer(void);
// Reads the window's framebuffer into <pattern % frame>.png (pattern is a printf format taking the frame number)
bool loopCaptureScreenshot(const char* pattern, int frame, int width, int height);
int loop(CommandLineArgs args, const char *argv0);
void freeCommandLineArgs(CommandLineArgs* args);

#endif
