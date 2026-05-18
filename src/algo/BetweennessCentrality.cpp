#include "BetweennessCentrality.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <queue>
#include <stack>
#include <vector>

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
