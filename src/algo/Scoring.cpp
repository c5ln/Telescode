#include "Scoring.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

// ── PageRank ──────────────────────────────────────────────────────────────────
//
// Power-method PageRank with dangling-node redistribution.
//
// Edge A→B means A imports/calls B, so PR(B) accumulates → B read first.
// Dangling nodes (out-degree 0) redistribute their mass uniformly across all nodes.
//
// PR(v) = (1-d)/N
//       + d * Σ_{u→v} PR(u)/out(u)
//       + d * (Σ_{dangling u} PR(u)) / N
//
// Convergence: Σ|PR_new - PR_old| < eps
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

std::vector<double> ScoreCombiner::combine(const std::vector<double>& pr,
                                            const std::vector<double>& bc,
                                            double alpha,
                                            double beta)
{
    const std::size_t N = pr.size();
    if (N == 0) return {};

    const double total = alpha + beta;
    const double w_pr  = alpha / total;
    const double w_bc  = beta  / total;

    auto pr_norm = minmax_normalize(pr);
    auto bc_norm = minmax_normalize(bc);

    std::vector<double> score(N);
    for (std::size_t i = 0; i < N; ++i)
        score[i] = w_pr * pr_norm[i] + w_bc * bc_norm[i];

    return score;
}
