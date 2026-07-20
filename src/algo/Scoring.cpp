#include "Scoring.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <numeric>
#include <queue>
#include <stack>


// 처음에 1/N으로 가중치를 나눠준 다음에 노드를 쭉 돌면서, out degree 방향의 node에게 가중치를 다시 나눠준다. (out degree 방향은 A가 B를 import한다면, A->B를 의미한다)
// 이를 반복하다가 이전 PageRank와 새로운 PageRank의 차이가 일정 수준 미안이면 종료한다. 현재는 eps가 1e-6 = 10^(-6)이다.

std::vector<double> PageRank::compute(const Graph& g, const AlgoConfig& cfg)
{
    const int N = g.size();
    if (N == 0) return {};

    const double d   = cfg.damping;
    const double eps = cfg.convergence_eps;

    std::vector<double> pr(N, 1.0 / N);
    std::vector<double> pr_new(N);
    std::vector<int>    out_deg(N);

    for (int u = 0; u < N; ++u)
        out_deg[u] = static_cast<int>(g.adj[u].size());

    for (int iter = 0; iter < cfg.max_iter; ++iter) {
        double dangling_sum = 0.0;
        for (int u = 0; u < N; ++u)
            if (out_deg[u] == 0)
                dangling_sum += pr[u];

        const double base = (1.0 - d) / N + d * dangling_sum / N;

        std::fill(pr_new.begin(), pr_new.end(), base);

        for (int u = 0; u < N; ++u) {
            if (out_deg[u] == 0) continue;
            const double contrib = d * pr[u] / out_deg[u];
            for (NodeId v : g.adj[u])
                pr_new[v] += contrib;
        }

        double diff = 0.0;
        for (int i = 0; i < N; ++i)
            diff += std::fabs(pr_new[i] - pr[i]);

        pr.swap(pr_new);

        if (diff < eps) break;
    }

    return pr;
}

// ── ScoreCombiner ─────────────────────────────────────────────────────────────

static std::vector<double> minmax_normalize(const std::vector<double>& v)
{
    if (v.empty()) return {};

    double lo = *std::min_element(v.begin(), v.end());
    double hi = *std::max_element(v.begin(), v.end());

    std::vector<double> out(v.size());
    if (hi == lo) {
        std::fill(out.begin(), out.end(), 0.5);
    } else {
        const double range = hi - lo;
        for (std::size_t i = 0; i < v.size(); ++i)
            out[i] = (v[i] - lo) / range;
    }
    return out;
}

// ── ComplexityScorer ────────────────────────────────────────────────────────

std::vector<double> percentile_rank_normalize(const std::vector<double>& values)
{
    const std::size_t N = values.size();
    if (N == 0) return {};
    if (N == 1) return {0.0};

    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());

    if (sorted.front() == sorted.back())
        return std::vector<double>(N, 0.5);

    std::vector<double> out(N);
    for (std::size_t i = 0; i < N; ++i) {
        auto it = std::lower_bound(sorted.begin(), sorted.end(), values[i]);
        std::size_t strictlyLower = static_cast<std::size_t>(it - sorted.begin());
        out[i] = static_cast<double>(strictlyLower) / static_cast<double>(N - 1);
    }
    return out;
}

// Normalizes a set of weights to sum to 1.0 if they don't already (+/- 0.001),
// logging a warning. Falls back to equal weights if the sum is ~0 (can't scale).
static void normalizeWeights(std::initializer_list<double*> weights, const char* label)
{
    double sum = 0.0;
    for (double* w : weights) sum += *w;
    if (std::fabs(sum - 1.0) <= 0.001) return;

    std::fprintf(stderr,
        "ComplexityScorer: %s weights sum to %.6f (expected 1.0) -- auto-normalizing\n",
        label, sum);

    if (std::fabs(sum) < 1e-9) {
        const double eq = 1.0 / static_cast<double>(weights.size());
        for (double* w : weights) *w = eq;
        return;
    }
    for (double* w : weights) *w /= sum;
}

std::vector<double> ComplexityScorer::compute(const std::vector<ComplexityFileMetrics>& inputs,
                                               const AlgoConfig& cfg)
{
    const std::size_t N = inputs.size();
    if (N == 0) return {};

    double w_cc       = cfg.complexity_w_cc;
    double w_nesting  = cfg.complexity_w_nesting;
    double w_loc      = cfg.complexity_w_loc;
    double w_inbound  = cfg.complexity_w_inbound;
    double w_outbound = cfg.complexity_w_outbound;
    normalizeWeights({&w_cc, &w_nesting, &w_loc, &w_inbound, &w_outbound}, "top-level");

    double cc_max_blend = cfg.complexity_cc_max_blend;
    double cc_avg_blend = cfg.complexity_cc_avg_blend;
    normalizeWeights({&cc_max_blend, &cc_avg_blend}, "cc pre-blend");

    double nesting_max_blend = cfg.complexity_nesting_max_blend;
    double nesting_avg_blend = cfg.complexity_nesting_avg_blend;
    normalizeWeights({&nesting_max_blend, &nesting_avg_blend}, "nesting pre-blend");

    std::vector<double> cc_raw(N), nesting_raw(N), loc_raw(N), inbound_raw(N), outbound_raw(N);
    for (std::size_t i = 0; i < N; ++i) {
        const ComplexityFileMetrics& m = inputs[i];
        cc_raw[i]      = cc_max_blend      * m.max_cyclomatic_complexity
                       + cc_avg_blend      * m.avg_cyclomatic_complexity;
        nesting_raw[i] = nesting_max_blend * m.max_block_depth
                       + nesting_avg_blend * m.avg_block_depth;
        loc_raw[i]      = static_cast<double>(m.logical_loc);
        inbound_raw[i]  = static_cast<double>(m.inbound);
        outbound_raw[i] = static_cast<double>(m.outbound);
    }

    std::vector<double> pct_cc       = percentile_rank_normalize(cc_raw);
    std::vector<double> pct_nesting  = percentile_rank_normalize(nesting_raw);
    std::vector<double> pct_loc      = percentile_rank_normalize(loc_raw);
    std::vector<double> pct_inbound  = percentile_rank_normalize(inbound_raw);
    std::vector<double> pct_outbound = percentile_rank_normalize(outbound_raw);

    std::vector<double> scores(N);
    for (std::size_t i = 0; i < N; ++i) {
        scores[i] = w_cc       * pct_cc[i]
                  + w_nesting  * pct_nesting[i]
                  + w_loc      * pct_loc[i]
                  + w_inbound  * pct_inbound[i]
                  + w_outbound * pct_outbound[i];
    }
    return scores;
}

int ComplexityScorer::computeAndWrite(sqlite3* db, const Graph& file_graph, const AlgoConfig& cfg)
{
    const char* selectSql = cfg.complexity_include_generated
        ? "SELECT file_id, max_cyclomatic_complexity, avg_cyclomatic_complexity, "
          "max_block_depth, avg_block_depth, logical_loc FROM file;"
        : "SELECT file_id, max_cyclomatic_complexity, avg_cyclomatic_complexity, "
          "max_block_depth, avg_block_depth, logical_loc FROM file WHERE is_generated = 0;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, selectSql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return rc;

    std::vector<ComplexityFileMetrics> inputs;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* fid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (!fid) continue;

        ComplexityFileMetrics m;
        m.file_id                   = fid;
        m.max_cyclomatic_complexity = sqlite3_column_int(stmt, 1);
        m.avg_cyclomatic_complexity = sqlite3_column_double(stmt, 2);
        m.max_block_depth           = sqlite3_column_int(stmt, 3);
        m.avg_block_depth           = sqlite3_column_double(stmt, 4);
        m.logical_loc               = sqlite3_column_int(stmt, 5);

        // Isolated files with no CALLS/INHERITS/IMPORTS edges still get a node
        // (GraphBuilder pre-populates every file_id), so a missing lookup here
        // means the file wasn't in the file table when the graph was built --
        // fall back to 0/0 rather than dropping the file from scoring.
        auto it = file_graph.id_to_node.find(m.file_id);
        if (it != file_graph.id_to_node.end()) {
            NodeId nid = it->second;
            m.inbound  = static_cast<int>(file_graph.radj[nid].size());
            m.outbound = static_cast<int>(file_graph.adj[nid].size());
        }

        inputs.push_back(std::move(m));
    }
    sqlite3_finalize(stmt);

    std::vector<double> scores = compute(inputs, cfg);

    rc = sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) return rc;

    sqlite3_stmt* upd = nullptr;
    rc = sqlite3_prepare_v2(db,
        "UPDATE file SET complexity_score = ? WHERE file_id = ?;", -1, &upd, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return rc;
    }

    for (std::size_t i = 0; i < inputs.size(); ++i) {
        sqlite3_bind_double(upd, 1, scores[i]);
        sqlite3_bind_text(upd, 2, inputs[i].file_id.c_str(), -1, SQLITE_STATIC);
        rc = sqlite3_step(upd);
        sqlite3_reset(upd);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(upd);
            sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
            return rc;
        }
    }
    sqlite3_finalize(upd);

    return sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
}

std::vector<double> ScoreCombiner::combine(const std::vector<double>& pr,
                                            const std::vector<double>& bc,
                                            double alpha,
                                            double beta)
{
    const std::size_t N = pr.size();
    if (N == 0 || bc.size() != N) return {};

    const double total = alpha + beta;
    const double w_pr  = (total != 0.0) ? alpha / total : 0.5;
    const double w_bc  = (total != 0.0) ? beta  / total : 0.5;

    auto pr_norm = minmax_normalize(pr);
    auto bc_norm = minmax_normalize(bc);

    std::vector<double> score(N);
    for (std::size_t i = 0; i < N; ++i)
        score[i] = w_pr * pr_norm[i] + w_bc * bc_norm[i];

    return score;
}

// 
//   핵심 수식

//   PR(v) = (1-d)/N
//         + d * Σ_{u→v} PR(u)/out(u)
//         + d * (Σ_{dangling u} PR(u)) / N

//   - d (damping factor): 임의로 다른 노드로 점프할 확률의 반대. 보통 0.85
//   - N: 전체 노드 수
//   - out(u): u의 아웃 엣지 수

//   ---
//   동작 방식

//   초기화 (28행): 모든 노드의 PR을 1/N으로 균등 분배.

//   반복 루프 (35-58행):

//   1. Dangling node 처리 (36-39행)
//   아웃 엣지가 없는 노드들은 PR을 어디에도 흘려보내지 못하므로, 이들의 합을 모아 전체 노드에 균등 재분배.
//   2. base 값 계산 (41행)
//   (1-d)/N (임의 점프 확률) + dangling node 재분배량. 모든 노드가 기본으로 받는 값.
//   3. 엣지를 통한 PR 전파 (45-49행)
//   u → v 엣지가 있으면, u의 PR을 아웃 엣지 수로 나눠 v에 분배.
//   4. 수렴 판정 (52-58행)
//   이전 PR과 새 PR의 L1 차이(Σ|PR_new - PR_old|)가 eps 미만이면 종료.

// ── Betweenness Centrality (Brandes) ────────────────────────────────────────

// Brandes single-source BC accumulation from source s.
// Adds unnormalized dependency deltas into bc[].
static void brandes_source(const Graph& g, NodeId s, std::vector<double>& bc)
{
    const int N = g.size();

    std::vector<int>                sigma(N, 0);
    std::vector<int>                dist(N, -1);
    std::vector<double>             delta(N, 0.0);
    std::vector<std::vector<NodeId>> pred(N);

    sigma[s] = 1;
    dist[s]  = 0;

    std::queue<NodeId> q;
    std::stack<NodeId> stk;
    q.push(s);

    while (!q.empty()) {
        NodeId v = q.front(); q.pop();
        stk.push(v);
        for (NodeId w : g.adj[v]) {
            if (dist[w] < 0) {
                dist[w] = dist[v] + 1;
                q.push(w);
            }
            if (dist[w] == dist[v] + 1) {
                sigma[w] += sigma[v];
                pred[w].push_back(v);
            }
        }
    }

    while (!stk.empty()) {
        NodeId w = stk.top(); stk.pop();
        for (NodeId v : pred[w]) {
            delta[v] += (static_cast<double>(sigma[v]) / sigma[w]) * (1.0 + delta[w]);
        }
        if (w != s) bc[w] += delta[w];
    }
}

std::vector<double> ExactBrandesStrategy::compute(const Graph& g) const
{
    const int N = g.size();
    std::vector<double> bc(N, 0.0);
    for (int s = 0; s < N; ++s)
        brandes_source(g, static_cast<NodeId>(s), bc);
    return bc;
}

std::vector<double> SamplingBrandesStrategy::compute(const Graph& g) const
{
    const int N = g.size();
    std::vector<double> bc(N, 0.0);
    if (N == 0) return bc;

    // Build a shuffled source order using a simple LCG seeded by `seed`.
    // This gives reproducible sampling regardless of platform RNG.
    std::vector<int> order(N);
    std::iota(order.begin(), order.end(), 0);

    uint64_t rng = seed;
    for (int i = N - 1; i > 0; --i) {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        int j = static_cast<int>(rng >> 33) % (i + 1);
        std::swap(order[i], order[j]);
    }

    const int samples = std::min(k, N);
    if (samples == 0) return bc;
    for (int i = 0; i < samples; ++i)
        brandes_source(g, static_cast<NodeId>(order[i]), bc);

    // Scale to approximate full BC
    if (samples < N) {
        const double scale = static_cast<double>(N) / samples;
        for (double& v : bc) v *= scale;
    }

    return bc;
}

std::vector<double> ZeroBCStrategy::compute(const Graph& g) const
{
    return std::vector<double>(static_cast<std::size_t>(g.size()), 0.0);
}

std::unique_ptr<IBCStrategy> make_bc_strategy(int V, const AlgoConfig& cfg, PassLevel level)
{
    if (level == PassLevel::File) {
        if (V < cfg.bc_p1_exact_v) {
            return std::make_unique<ExactBrandesStrategy>();
        } else if (V < cfg.bc_p1_large_v) {
            int k = std::max(cfg.bc_k_min,
                             static_cast<int>(std::ceil(std::sqrt(static_cast<double>(V)))));
            k = std::min(k, V);
            return std::make_unique<SamplingBrandesStrategy>(k, cfg.bc_seed);
        } else {
            int k = std::min(cfg.bc_p1_fixed_k, V);
            return std::make_unique<SamplingBrandesStrategy>(k, cfg.bc_seed);
        }
    } else {
        if (!cfg.enable_p2_bc) {
            return std::make_unique<ZeroBCStrategy>();
        }
        if (V < cfg.bc_p2_exact_v) {
            return std::make_unique<ExactBrandesStrategy>();
        } else if (V < cfg.bc_p2_large_v) {
            int k = std::max(cfg.bc_k_min,
                             static_cast<int>(std::ceil(std::sqrt(static_cast<double>(V)))));
            k = std::min(k, V);
            return std::make_unique<SamplingBrandesStrategy>(k, cfg.bc_seed);
        } else {
            int k = std::min(cfg.bc_p2_fixed_k, V);
            return std::make_unique<SamplingBrandesStrategy>(k, cfg.bc_seed);
        }
    }
}