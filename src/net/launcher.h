#ifndef _BS_LAUNCHER_H_
#define _BS_LAUNCHER_H_

// Graphical launcher shown when the program is started without arguments: pick a game (auto-detected or typed path),
// then host a server, join one, or play offline. Returns the process exit code.
int launcherMain(const char* argv0);

#endif
