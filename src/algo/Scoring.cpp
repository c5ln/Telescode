#include "Scoring.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>


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