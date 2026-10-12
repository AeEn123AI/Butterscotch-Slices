#include "args.h"
#include <loop.h>
#include <getopt.h>
#ifdef ENABLE_NETPLAY
#include "net_server.h"
#include "net_client.h"
#include "launcher.h"
#endif

/* For SDL_main */
#if defined(USE_SDL1)
#include <SDL/SDL_main.h>
#elif defined(USE_SDL2)
#include <SDL2/SDL_main.h>
#elif defined(USE_SDL3)
#include <SDL3/SDL_main.h>
#endif

static bool logColour;

void platformLog(const logType type, const char *format, va_list va) {
    FILE *out = stderr;
    const char* colourPrefix = ANSI_COLOUR_CODE_RESET;
    const char* textPrefix = "";
    switch (type) {
        case LOG_TYPE_NORMAL:
            out = stdout;
            break;
        case LOG_TYPE_WARNING:
            colourPrefix = ANSI_COLOUR_CODE_BOLD_YELLOW;
            textPrefix = "Warning: ";
            break;
        case LOG_TYPE_ERROR:
            colourPrefix = ANSI_COLOUR_CODE_BOLD_RED;
            textPrefix = "Error: ";
            break;
        case LOG_TYPE_DEBUG:
            colourPrefix = ANSI_COLOUR_CODE_BOLD_PURPLE;
            textPrefix = "Debug: ";
            break;
    }

    if (logColour) fputs(colourPrefix, out);
    fputs(textPrefix, out);
    if (logColour) fputs(ANSI_COLOUR_CODE_RESET, out);
    vfprintf(out, format, va);
}

int main(int argc, char* argv[]) {
    setbuf(stderr, NULL);

#ifdef ENABLE_NETPLAY
    // Double-clicked / started without any arguments: show the launcher
    if (argc == 1) {
        logColour = true;
        return launcherMain(argv[0]);
    }
#endif

    CommandLineArgs args;
    parseCommandLineArgs(&args, argc, argv);
    logColour = !args.disableLogColours;
#ifdef ENABLE_NETPLAY
    if (args.netMode != NET_MODE_STANDALONE) setvbuf(stdout, NULL, _IOLBF, 0); // keep server / client logs readable when redirected to a file
#ifdef ENABLE_NET_FUZZ
    if (args.netFuzzCorpus != nullptr) {
        int ret = NetClient_fuzz(args, args.netFuzzCorpus, args.netFuzzIterations > 0 ? args.netFuzzIterations : 10000, (unsigned) args.netFuzzSeed);
        freeCommandLineArgs(&args);
        return ret;
    }
#endif
    if (args.netMode == NET_MODE_SERVER) {
        int ret = NetServer_run(args, argv[0]);
        freeCommandLineArgs(&args);
        return ret;
    }
    if (args.netMode == NET_MODE_CLIENT) {
        int ret = NetClient_run(args, argv[0]);
        freeCommandLineArgs(&args);
        return ret;
    }
#endif
    int ret = loop(args, argv[0]);
    freeCommandLineArgs(&args);
    return ret;
}
