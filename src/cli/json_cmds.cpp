// src/cli/json_cmds.cpp
// `Telescode graph` / `sequence` / `analyze` -- read a database, print JSON.
//
// The three differ only in which slice of the snapshot they serialize, so they
// share one body. None of them initialises SDL, creates a window, or touches a
// renderer; they are what "headless" means in practice for this project.

#include "cli/ts_cli.h"

#include "core/AnalysisService.h"
#include "core/json/AnalysisJson.h"

#include <cstdio>
#include <exception>
#include <string>

namespace TS {
namespace {

// Which serializer to apply to the snapshot.
enum class Slice { Graph, Sequence, Full };

std::string render(Slice slice, const AnalysisSnapshot& snap, bool pretty)
{
    switch (slice) {
        case Slice::Graph:    return GraphToJson(snap, pretty);
        case Slice::Sequence: return ReadingSequenceToJson(snap, pretty);
        case Slice::Full:     break;
    }
    return AnalysisSnapshotToJson(snap, pretty);
}

// Writes to out_path, or stdout when it is empty. Returns false on a write
// error, which must not be silent: a truncated JSON file that looks like it was
// produced successfully is worse than no file.
bool emit(const std::string& text, const std::string& out_path)
{
    if (out_path.empty()) {
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fputc('\n', stdout);
        return std::fflush(stdout) == 0;
    }

    std::FILE* f = std::fopen(out_path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "Telescode: cannot open '%s' for writing\n", out_path.c_str());
        return false;
    }
    const size_t wrote = std::fwrite(text.data(), 1, text.size(), f);
    std::fputc('\n', f);
    const bool ok = (wrote == text.size()) && (std::fclose(f) == 0);
    if (!ok)
        std::fprintf(stderr, "Telescode: failed writing '%s'\n", out_path.c_str());
    else
        std::fprintf(stderr, "[%s] wrote %zu bytes\n", out_path.c_str(), text.size());
    return ok;
}

int run(Slice slice, int argc, char* argv[], const char* usage)
{
    JsonCmdOptions opts;
    if (!ParseJsonCmdOptions(argc, argv, usage, opts)) return 2;

    try {
        AnalysisOptions aopts;
        aopts.run_algo = opts.run_algo;

        const AnalysisSnapshot snap = AnalysisService::Load(opts.db_path, aopts);
        return emit(render(slice, snap, opts.pretty), opts.out_path) ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Telescode: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "Telescode: unknown error\n");
        return 1;
    }
}

} // anonymous namespace

int CmdGraph(int argc, char* argv[])
{
    return run(Slice::Graph, argc, argv,
               "Usage: Telescode graph <db_path> [--json] [--pretty] [--algo] [-o <file>]");
}

int CmdSequence(int argc, char* argv[])
{
    return run(Slice::Sequence, argc, argv,
               "Usage: Telescode sequence <db_path> [--json] [--pretty] [--algo] [-o <file>]");
}

int CmdAnalyze(int argc, char* argv[])
{
    return run(Slice::Full, argc, argv,
               "Usage: Telescode analyze <db_path> [--json] [--pretty] [--algo] [-o <file>]");
}

} // namespace TS
