#ifndef _BS_NET_SERVER_H_
#define _BS_NET_SERVER_H_

#include "platformdefs.h"

// Runs the game as a network server: executes all game logic, accepts any number of clients, merges their input and
// streams the resulting draw / audio calls to them. Returns the process exit code.
int NetServer_run(CommandLineArgs args, const char* argv0);

#endif
