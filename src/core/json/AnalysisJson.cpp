// src/core/json/AnalysisJson.cpp

#include "core/json/AnalysisJson.h"
#include "core/json/JsonWriter.h"

namespace TS {
namespace {

const char* edgeTypeName(CDEdgeType t)
{
    // Spelled out rather than emitted as the enum's integer value: the number is
    // a C++ implementation detail and would silently change meaning if the enum
    // were ever reordered.
    switch (t) {
        case CDEdgeType::Dependency:  return "dependency";
        case CDEdgeType::Association: return "association";
    }
    return "unknown";
}

void writeSchema(JsonWriter& w)
{
    // Versioned so a frontend can tell which shape it is looking at without
    // guessing from the keys present.
    w.member("schemaVersion", 1);
}

void writeTotals(JsonWriter& w, const AnalysisTotals& t)
{
    w.key("totals");
    w.beginObject();
    w.member("fileCount",      t.file_count);
    w.member("classCount",     t.class_count);
    w.member("classEdgeCount", t.class_edge_count);
    w.member("fileNodeCount",  t.file_node_count);
    w.member("fileEdgeCount",  t.file_edge_count);
    w.member("funcNodeCount",  t.func_node_count);
    w.member("funcEdgeCount",  t.func_edge_count);
    w.member("sequenceCount",  t.sequence_count);
    w.endObject();
}

// Files carry both the parser's rollup metrics and the scorer's complexity_score.
// They are nested under "metrics" so a consumer can tell identity from measurement.
void writeFiles(JsonWriter& w, const AnalysisSnapshot& snap)
{
    w.key("files");
    w.beginArray();
    for (const FileMetricsRow& f : snap.files) {
        w.beginObject();
        w.member("fileId",      f.file_id);
        w.member("fileName",    f.file_name);
        w.member("language",    f.language);
        w.member("isGenerated", f.is_generated);
        w.key("metrics");
        w.beginObject();
        w.member("rawLoc",                  f.raw_loc);
        w.member("logicalLoc",              f.logical_loc);
        w.member("maxCyclomaticComplexity", f.max_cyclomatic_complexity);
        w.member("avgCyclomaticComplexity", f.avg_cyclomatic_complexity);
        w.member("maxBlockDepth",           f.max_block_depth);
        w.member("avgBlockDepth",           f.avg_block_depth);
        w.member("maxFunctionLoc",          f.max_function_loc);
        w.member("avgFunctionLoc",          f.avg_function_loc);
        w.member("complexityScore",         f.complexity_score);
        w.endObject();
        w.endObject();
    }
    w.endArray();
}

void writeFileGraph(JsonWriter& w, const AnalysisSnapshot& snap)
{
    w.key("fileGraph");
    w.beginObject();

    w.key("nodes");
    w.beginArray();
    for (const FileDegree& d : snap.file_degrees) {
        w.beginObject();
        w.member("fileId",   d.file_id);
        w.member("inbound",  d.inbound);
        w.member("outbound", d.outbound);
        w.endObject();
    }
    w.endArray();

    w.key("edges");
    w.beginArray();
    for (const FileEdge& e : snap.file_edges) {
        w.beginObject();
        w.member("source", e.source_file_id);
        w.member("target", e.target_file_id);
        w.endObject();
    }
    w.endArray();

    w.endObject();
}

// The class graph. node_id is imnodes' integer handle in the viewer; classId is
// the stable database key, so that is what edges reference here. Callers that
// need to join back to the viewer still have "nodeId" available.
void writeClasses(JsonWriter& w, const AnalysisSnapshot& snap)
{
    const std::vector<CDNode>& nodes = snap.classes.nodes;

    w.key("classes");
    w.beginArray();
    for (const CDNode& n : nodes) {
        w.beginObject();
        w.member("classId",   n.class_id);
        w.member("nodeId",    n.node_id);
        w.member("fileId",    n.file_id);
        w.member("className", n.class_name);
        w.member("package",   n.package);

        w.key("fields");
        w.beginArray();
        for (const CDField& f : n.fields) {
            w.beginObject();
            w.member("access", std::string(1, f.access));
            w.member("name",   f.name);
            w.endObject();
        }
        w.endArray();

        w.key("methods");
        w.beginArray();
        for (const CDMethod& m : n.methods) {
            w.beginObject();
            w.member("access",     std::string(1, m.access));
            w.member("name",       m.name);
            w.member("params",     m.params);
            w.member("returnType", m.ret_type);
            w.endObject();
        }
        w.endArray();

        w.endObject();
    }
    w.endArray();

    w.key("classEdges");
    w.beginArray();
    for (const CDEdge& e : snap.classes.edges) {
        const size_t s = static_cast<size_t>(e.src_node_id);
        const size_t d = static_cast<size_t>(e.dst_node_id);
        w.beginObject();
        w.member("edgeId", e.edge_id);
        // node_id is assigned as the row index by the builder, so this indexing
        // holds; the guard is for a graph that was edited after it was built.
        w.member("source", s < nodes.size() ? nodes[s].class_id : std::string());
        w.member("target", d < nodes.size() ? nodes[d].class_id : std::string());
        w.member("type",   edgeTypeName(e.type));
        w.endObject();
    }
    w.endArray();
}

void writeReadingSequence(JsonWriter& w, const AnalysisSnapshot& snap)
{
    w.key("readingSequence");
    w.beginArray();
    for (const ReadingSequenceRow& r : snap.reading_sequence) {
        const bool is_file = (r.entity_type == "file");
        w.beginObject();
        w.member("entityId",   r.entity_id);
        w.member("entityType", r.entity_type);
        w.member("fileId",     r.file_id);
        // Exactly one rank is meaningful per row; the other column is NULL in the
        // table, and reporting a 0 for it would read as a real rank.
        w.key("fileRank");
        if (is_file) w.value(r.file_rank);  else w.valueNull();
        w.key("localRank");
        if (is_file) w.valueNull();         else w.value(r.local_rank);
        w.key("scores");
        w.beginObject();
        w.member("pagerank", r.pagerank_score);
        w.member("betweenness", r.bc_score);
        w.member("combined", r.combined_score);
        w.endObject();
        w.endObject();
    }
    w.endArray();
}

} // anonymous namespace

std::string AnalysisSnapshotToJson(const AnalysisSnapshot& snap, bool pretty)
{
    JsonWriter w(pretty);
    w.beginObject();
    writeSchema(w);
    w.member("dbPath", snap.db_path);
    writeTotals(w, snap.totals);
    writeFiles(w, snap);
    writeFileGraph(w, snap);
    writeClasses(w, snap);
    writeReadingSequence(w, snap);
    w.endObject();
    return w.str();
}

std::string GraphToJson(const AnalysisSnapshot& snap, bool pretty)
{
    JsonWriter w(pretty);
    w.beginObject();
    writeSchema(w);
    w.member("dbPath", snap.db_path);
    writeTotals(w, snap.totals);
    writeFiles(w, snap);
    writeFileGraph(w, snap);
    writeClasses(w, snap);
    w.endObject();
    return w.str();
}

std::string ReadingSequenceToJson(const AnalysisSnapshot& snap, bool pretty)
{
    JsonWriter w(pretty);
    w.beginObject();
    writeSchema(w);
    w.member("dbPath", snap.db_path);
    writeTotals(w, snap.totals);
    writeReadingSequence(w, snap);
    w.endObject();
    return w.str();
}

} // namespace TS
