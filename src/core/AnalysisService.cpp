// src/core/AnalysisService.cpp

#include "core/AnalysisService.h"

#include "algo/AlgoDbWriter.h"
#include "algo/AlgoRunner.h"
#include "algo/Graph.h"
#include "core/diagram/cd_builder.h"

#include <sqlite3.h>
#include <stdexcept>
#include <string>

namespace TS {
namespace {

// Degrees and edges are read straight off the built Graph rather than
// re-queried, so they agree with the graph the algorithms actually ran on --
// including the same inbound/outbound convention ComplexityScorer uses
// (inbound = radj, outbound = adj; see Scoring.cpp).
void collectFileGraph(const Graph& fg,
                      std::vector<FileDegree>& degrees,
                      std::vector<FileEdge>&   edges)
{
    const int n = fg.size();
    degrees.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const NodeId nid = static_cast<NodeId>(i);
        FileDegree d;
        d.file_id  = fg.node_to_id[nid];
        d.inbound  = static_cast<int>(fg.radj[nid].size());
        d.outbound = static_cast<int>(fg.adj[nid].size());
        degrees.push_back(std::move(d));
    }

    // Node order is insertion order from GraphBuilder, which reads the link
    // table in a fixed order, so this listing is stable run to run.
    for (int i = 0; i < n; ++i) {
        const NodeId nid = static_cast<NodeId>(i);
        for (NodeId t : fg.adj[nid])
            edges.push_back({ fg.node_to_id[nid], fg.node_to_id[t] });
    }
}

int edgeCount(const Graph& g)
{
    int total = 0;
    for (int i = 0; i < g.size(); ++i)
        total += static_cast<int>(g.adj[static_cast<NodeId>(i)].size());
    return total;
}

} // anonymous namespace

AnalysisSnapshot AnalysisService::Load(const std::string& dbPath,
                                      const AnalysisOptions& opts,
                                      const AlgoConfig& cfg)
{
    // Recompute first, through the very same call the viewer makes at startup,
    // then read the results back below. Running it before the read-only handle
    // opens keeps the two from contending for the write lock.
    if (opts.run_algo)
        AlgoRunner::run(dbPath.c_str(), cfg);

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        const std::string msg = db ? sqlite3_errmsg(db) : "cannot open database";
        sqlite3_close(db);
        throw std::runtime_error("AnalysisService: " + dbPath + ": " + msg);
    }

    AnalysisSnapshot snap;
    snap.db_path = dbPath;

    GraphBuilderResult gbr = GraphBuilder::build(db);
    collectFileGraph(gbr.file_graph, snap.file_degrees, snap.file_edges);

    snap.files            = QueryFileMetrics(db);
    snap.classes          = BuildCDGraph(db);
    snap.reading_sequence = QueryReadingSequence(db);

    snap.totals.file_count       = static_cast<int>(snap.files.size());
    snap.totals.class_count      = static_cast<int>(snap.classes.nodes.size());
    snap.totals.class_edge_count = static_cast<int>(snap.classes.edges.size());
    snap.totals.file_node_count  = gbr.file_graph.size();
    snap.totals.file_edge_count  = edgeCount(gbr.file_graph);
    snap.totals.func_node_count  = gbr.func_graph.size();
    snap.totals.func_edge_count  = edgeCount(gbr.func_graph);
    snap.totals.sequence_count   = static_cast<int>(snap.reading_sequence.size());

    sqlite3_close(db);
    return snap;
}

AnalysisSnapshot AnalysisService::Load(const std::string& dbPath,
                                      const AnalysisOptions& opts)
{
    // Read the stored config only when something will use it -- the overload
    // above consults cfg solely to run the algo pass.
    //
    // This is not a micro-optimisation. AlgoDbWriter::loadConfig goes through
    // initDb, which opens the database read-write and will create missing
    // tables, apply schema migrations, switch journal_mode to WAL, and create
    // the file outright if the path does not exist. Fetching a config that is
    // then discarded would let `graph`, `sequence` and `analyze` rewrite the
    // database they were asked only to report on -- and turn a mistyped path
    // into a new empty database plus a successful-looking empty result.
    if (!opts.run_algo)
        return Load(dbPath, opts, AlgoConfig{});

    return Load(dbPath, opts, AlgoDbWriter::loadConfig(dbPath.c_str()));
}

} // namespace TS
