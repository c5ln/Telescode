// src/cli/headless_main.cpp
// Entry point for TelescodeHeadless -- the core with no graphical layer.
//
// This binary links telescode_core and no graphical library, so it runs on a
// machine with no display.

#include "cli/ts_cli.h"

#include <cstdio>
#include <cstring>

namespace {

void PrintUsage(std::FILE* out)
{
    std::fprintf(out,
        "Telescode (headless) -- a telescope for your codebase.\n"
        "\n"
        "Usage:\n");
    TS::PrintCoreUsage(out);
}

} // anonymous namespace

int main(int argc, char** argv)
{
    if (argc <= 1) { PrintUsage(stderr); return 2; }

    if (std::strcmp(argv[1], "--help") == 0 ||
        std::strcmp(argv[1], "-h")     == 0) { PrintUsage(stdout); return 0; }

    int exit_code = 0;
    if (TS::DispatchCoreCommand(argc, argv, exit_code)) return exit_code;

    // A bare path means nothing here -- there is no viewer to open it in, so say
    // so rather than appearing to do something.
    std::fprintf(stderr, "Telescode: unknown command '%s'\n\n", argv[1]);
    PrintUsage(stderr);
    return 2;
}
