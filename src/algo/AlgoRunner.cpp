#include "AlgoRunner.h"
#include "AlgoDbWriter.h"
#include "BetweennessCentrality.h"
#include "GraphBuilder.h"
#include "ReadingSequencer.h"
#include "Scoring.h"
#include "db/db.h"

#include <algorithm>
#include <climits>
#include <sqlite3.h>
#include <unordered_map>
#include <vector>

// ── AlgoPass::run ─────────────────────────────────────────────────────────────

AlgoPassResult AlgoPass::run(const Graph& g, const AlgoConfig& cfg,
                              const std::vector<int>& loc_hint)
{
    auto pr  = PageRank::compute(g, cfg);
    auto bc  = make_bc_strategy(g.size(), cfg, level())->compute(g);
    auto sc  = ScoreCombiner::combine(pr, bc, cfg.alpha, cfg.beta);
    auto seq = ReadingSequencer::sequence(g, sc, loc_hint);
    return {std::move(pr), std::move(bc), std::move(sc), std::move(seq)};
}

// ── merge ─────────────────────────────────────────────────────────────────────

AlgoRunResult AlgoRunner::merge(const AlgoPassResult&    file_result,
                                 const Graph&             file_graph,
                                 const AlgoPassResult&    func_result,
                                 const Graph&             func_graph,
                                 const GraphBuilderResult& gbr)
{
    AlgoRunResult out;

    // ── Pass 1: file entries ─────────────────────────────────────────────────
    // Only include nodes that exist in the file table (file_loc_map keys).
    // file_graph may contain external module names (e.g. "os", "pytest") from
    // IMPORTS targets that are not real project files → skip them to avoid FK violations.
    int file_rank_counter = 0;
    for (int i = 0; i < static_cast<int>(file_result.seq.size()); ++i) {
        NodeId nid = file_result.seq[i];
        if (nid >= static_cast<NodeId>(file_graph.node_to_id.size())) continue;
        const std::string& fid = file_graph.node_to_id[nid];
        if (gbr.file_loc_map.find(fid) == gbr.file_loc_map.end()) continue;

        ++file_rank_counter;
        ReadingEntry e;
        e.entity_id      = fid;
        e.entity_type    = "file";
        e.file_id        = fid;
        e.file_rank      = file_rank_counter;  // 1-indexed, gaps removed
        e.local_rank     = 0;
        e.pagerank_score = nid < file_result.pr.size() ? file_result.pr[nid] : 0.0;
        e.bc_score       = nid < file_result.bc.size() ? file_result.bc[nid] : 0.0;
        e.combined_score = nid < file_result.sc.size() ? file_result.sc[nid] : 0.0;
        out.entries.push_back(std::move(e));
    }

    // ── Pass 2: function/class entries ───────────────────────────────────────

    // Build global_rank map: entity_id -> position in func_result.seq (0-indexed)
    std::unordered_map<std::string, int> global_rank;
    global_rank.reserve(func_result.seq.size());
    for (int i = 0; i < static_cast<int>(func_result.seq.size()); ++i) {
        NodeId nid = func_result.seq[i];
        if (nid < static_cast<NodeId>(func_graph.node_to_id.size()))
            global_rank[func_graph.node_to_id[nid]] = i;
    }

    // Group entities by file_id
    std::unordered_map<std::string, std::vector<std::string>> file_to_entities;
    for (int i = 0; i < func_graph.size(); ++i) {
        const std::string& eid = func_graph.node_to_id[static_cast<NodeId>(i)];
        auto it = gbr.entity_file_map.find(eid);
        if (it != gbr.entity_file_map.end())
            file_to_entities[it->second].push_back(eid);
    }

    // 파일별로 엔티티를 global_rank ASC 정렬 → local_rank 1, 2, 3, ... 부여
    // global_rank가 같을 때(Pass 2 그래프에 없던 고립 엔티티 등) start_line ASC로 보조 정렬
    for (auto& [fid, entities] : file_to_entities) {
        std::sort(entities.begin(), entities.end(),
            [&](const std::string& a, const std::string& b) {
                int ra = global_rank.count(a) ? global_rank.at(a) : INT_MAX;
                int rb = global_rank.count(b) ? global_rank.at(b) : INT_MAX;
                if (ra != rb) return ra < rb;
                int la = gbr.entity_start_line.count(a) ? gbr.entity_start_line.at(a) : 0;
                int lb = gbr.entity_start_line.count(b) ? gbr.entity_start_line.at(b) : 0;
                return la < lb;
            });

        for (int local = 0; local < static_cast<int>(entities.size()); ++local) {
            const std::string& eid = entities[local];

            // Look up NodeId in func_graph for scores
            NodeId nid = func_graph.node_to_id.size();  // sentinel
            auto nit = func_graph.id_to_node.find(eid);
            if (nit != func_graph.id_to_node.end()) nid = nit->second;

            std::string etype = "function";
            auto tit = gbr.entity_type_map.find(eid);
            if (tit != gbr.entity_type_map.end()) etype = tit->second;

            ReadingEntry e;
            e.entity_id      = eid;
            e.entity_type    = etype;
            e.file_id        = fid;
            e.file_rank      = 0;
            e.local_rank     = local + 1;  // 1-indexed
            e.pagerank_score = nid < func_result.pr.size() ? func_result.pr[nid] : 0.0;
            e.bc_score       = nid < func_result.bc.size() ? func_result.bc[nid] : 0.0;
            e.combined_score = nid < func_result.sc.size() ? func_result.sc[nid] : 0.0;
            out.entries.push_back(std::move(e));
        }
    }

    return out;
}

// ── AlgoRunner::run ───────────────────────────────────────────────────────────

AlgoRunResult AlgoRunner::run(const char* dbPath, const AlgoConfig& cfg)
{
    sqlite3* db = nullptr;
    if (initDb(dbPath, &db) != SQLITE_OK) return {};

    auto gbr = GraphBuilder::build(db);

    // Build loc_hint for file-level pass (LOC descending tie-break)
    const Graph& fg = gbr.file_graph;
    std::vector<int> file_loc_hint(fg.size(), 0);
    for (int i = 0; i < fg.size(); ++i) {
        const std::string& fid = fg.node_to_id[static_cast<NodeId>(i)];
        auto it = gbr.file_loc_map.find(fid);
        if (it != gbr.file_loc_map.end()) file_loc_hint[i] = it->second;
    }

    auto file_result = FilePass{}.run(gbr.file_graph, cfg, file_loc_hint);
    auto func_result = FunctionPass{}.run(gbr.func_graph, cfg);

    sqlite3_close(db);

    auto merged = merge(file_result, gbr.file_graph,
                        func_result, gbr.func_graph, gbr);

    AlgoDbWriter::write(dbPath, merged, cfg);
    return merged;
}
