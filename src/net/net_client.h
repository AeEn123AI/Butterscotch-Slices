#ifndef _BS_NET_CLIENT_H_
#define _BS_NET_CLIENT_H_

#include "platformdefs.h"

// Connects to a server (args.netConnect), loads the matching local copy of the game and then only renders, plays sound and
// forwards input. Returns the process exit code.
int NetClient_run(CommandLineArgs args, const char* argv0);
// Human readable reason the last NetClient_run() ended abnormally ("" if it ended normally); used by the launcher
const char* NetClient_lastMessage(void);
#ifdef ENABLE_NET_FUZZ
// Test-only: feeds mutated copies of recorded server messages (BUTTERSCOTCH_NET_RECORD) to the decoders
int NetClient_fuzz(CommandLineArgs args, const char* corpusPath, int iterations, unsigned seed);
#endif

#endif
