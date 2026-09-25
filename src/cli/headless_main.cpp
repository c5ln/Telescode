// src/cli/headless_main.cpp
// Entry point for TelescodeHeadless -- the core with no graphical layer.
//
// This binary links telescode_core and nothing from SDL, Dear ImGui or imnodes.
// That is deliberate and is the check that the separation actually holds: if
// core code ever reacquires a UI dependency, this target stops linking, and it
// does so at build time rather than the first time someone runs it on a machine
// with no display.

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
    std::fprintf(out,
        "\n"
        "This binary has no viewer. Use the Telescode executable for that.\n");
}

} // anonymous namespace

int main(int argc, char** argv)
{
    if (argc <= 1) { PrintUsage(stderr); return 2; }

    if (std::strcmp(argv[1], "--help") == 0 ||
        std::strcmp(argv[1], "-h")     == 0) { PrintUsage(stdout); return 0; }

    int exit_code = 0;
    if (TS::DispatchCoreCommand(argc, argv, exit_code)) return exit_code;

    // Unlike the viewer, a bare path means nothing here -- there is no window to
    // open it in, so say so rather than appearing to do something.
    std::fprintf(stderr, "Telescode: unknown command '%s'\n\n", argv[1]);
    PrintUsage(stderr);
    return 2;
}
