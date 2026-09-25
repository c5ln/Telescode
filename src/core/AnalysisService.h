// src/core/AnalysisService.h
// One headless entry point for "analyse this database and hand back the result".
//
// This is composition, not computation. Every number in an AnalysisSnapshot is
// produced by the implementation that already owned it:
//
//   file graph / function graph   GraphBuilder      (src/algo/Graph.cpp)
//   reading order + scores        AlgoRunner        (src/algo/AlgoRunner.cpp)
//   complexity_score             ComplexityScorer  (src/algo/Scoring.cpp)
//   class structure + relations   BuildCDGraph      (src/core/diagram/cd_builder.cpp)
//   per-file rollup metrics       the `file` table  (written by the parser)
//
// Nothing here re-derives any of it. Serialization lives in core/json, so the
// snapshot stays independent of the format it is eventually rendered into.

#pragma once

#include "algo/AlgoConfig.h"
#include "core/FileMetricsQuery.h"
#include "core/ReadingSequenceQuery.h"
#include "core/diagram/cd_model.h"

#include <string>
#include <vector>

namespace TS {

// A directed edge between two file_ids, from the file-level graph.
struct FileEdge {
    std::string source_file_id;
    std::string target_file_id;
};

// In/out degree of a file in the file-level graph. These are the same counts the
// complexity scorer consumes as its inbound / outbound inputs.
struct FileDegree {
    std::string file_id;
    int inbound  = 0;
    int outbound = 0;
};

// Whole-graph totals. Counts only -- no derived score lives here.
struct AnalysisTotals {
    int file_count        = 0;
    int class_count       = 0;
    int class_edge_count  = 0;
    int file_node_count   = 0;   // nodes in the file-level graph
    int file_edge_count   = 0;
    int func_node_count   = 0;   // nodes in the function/class-level graph
    int func_edge_count   = 0;
    int sequence_count    = 0;   // reading_sequence rows
};

// Note on what is deliberately absent: a snapshot carries no node geometry.
// The layout code (core/diagram/cd_layout) is core and runs headlessly, but the
// metrics it needs -- row heights, padding, font-derived widths -- are style
// values owned by the renderer. Emitting positions here would mean restating
// those constants in core and letting the two drift. A consumer that wants a
// picture calls the layout itself with its own metrics.
struct AnalysisSnapshot {
    std::string                     db_path;
    AnalysisTotals                  totals;
    std::vector<FileMetricsRow>     files;
    std::vector<FileDegree>         file_degrees;
    std::vector<FileEdge>           file_edges;
    CDGraph                         classes;          // nodes + edges, unlaid-out
    std::vector<ReadingSequenceRow> reading_sequence;
};

// How much work Load() should do.
struct AnalysisOptions {
    // Recompute the reading sequence and complexity scores before reading them
    // back, by running the same AlgoRunner pass the viewer runs at startup.
    //
    // false reports whatever the database already holds and does not write to it
    // at all: the database is opened read-only, and the stored config -- whose
    // loader would initialise and migrate the schema on the way in -- is not
    // consulted, since only the algo pass needs it.
    bool run_algo = false;
};

class AnalysisService {
public:
    // Opens dbPath, gathers a snapshot, closes it. Throws std::runtime_error if
    // the database cannot be opened. A database missing the algo tables yields a
    // snapshot with an empty reading_sequence rather than an error, matching how
    // the viewer treats one.
    static AnalysisSnapshot Load(const std::string& dbPath,
                                 const AnalysisOptions& opts,
                                 const AlgoConfig& cfg);

    // Load() with the config the database itself carries.
    static AnalysisSnapshot Load(const std::string& dbPath,
                                 const AnalysisOptions& opts);
};

} // namespace TS
