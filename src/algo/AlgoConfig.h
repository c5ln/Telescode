#pragma once

#include <cstdint>

// reading_sequence_config 스키마 버전.
// 이미 DB에 저장된 값을 **무시하고** 새 기본값을 강제해야 할 때만 올린다.
// 오래된 빌드가 seed한 DB는 낮은 버전을 들고 있고, loadConfig가 해당 키의
// 저장값을 버린다.
//   1 -> 2: gamma 기본값 1.5 -> 0.2
inline constexpr int kAlgoConfigVersion = 2;

// 읽기 순서 알고리즘 전체에 걸친 하이퍼파라미터 집합.
// DB 동기화는 AlgoDbWriter::loadConfig / write 를 통해 이루어진다.
struct AlgoConfig {
    // combined = (alpha*PR_norm + beta*BC_norm + gamma*ease) / (alpha+beta+gamma)
    // where ease = 1 - complexity_score. gamma is only applied at the file level
    // (Pass 2 has no complexity_score).
    //
    // Importance decides the order; understanding cost only breaks near-ties.
    // gamma=0.2 gives ease 1/6 of the weight, enough to separate files PR and BC
    // score alike and not enough to reorder files they rank differently.
    // Raising gamma until "read the easy files first" wins outright inverts the
    // tool: the sequence fills up with trivial leaves and the files that carry
    // the codebase sink. gamma is a hyperparameter, so its value belongs to
    // Phase 4 CV on training instances -- never to a summary statistic measured
    // on the corpus being diagnosed.
    double   alpha             = 0.6;
    double   beta              = 0.4;
    double   gamma             = 0.2;

    // How PR/BC are mapped to [0,1] before the weighted sum.
    //   0 = min-max, 1 = percentile rank.
    // min-max squashes a 187x-span PageRank distribution against the bottom of
    // the range, collapsing most files onto an identical score; percentile rank
    // is position-based and also matches what ComplexityScorer already does.
    int      score_norm_mode   = 1;

    // complexity_score stand-in for files the scorer skipped (is_generated=1) or
    // graph nodes with no `file` row. 0.5 = neutral; the schema default is 0.0,
    // which would otherwise read as "trivially easy" and sort them to the front.
    double   complexity_neutral = 0.5;

    // PageRank (power method)
    double   damping           = 0.85;  // random surfer가 링크를 따를 확률
    int      max_iter          = 100;
    double   convergence_eps   = 1e-6;  // Σ|PR_new - PR_old| < eps

    // BC Pass 1 (파일 레벨) 전략 선택 
    // V < bc_p1_exact_v              → 정확한 Brandes (전체 소스 노드)
    // bc_p1_exact_v ≤ V < bc_p1_large_v → k = max(bc_k_min, sqrt(V)) 샘플링
    // V ≥ bc_p1_large_v              → k = bc_p1_fixed_k 고정 샘플링
    int      bc_p1_exact_v     = 200;
    int      bc_p1_large_v     = 2000;
    int      bc_p1_fixed_k     = 64;

    //  BC Pass 2 (함수/클래스 레벨) 전략 선택 
    bool     enable_p2_bc      = true;  // false 시 Pass 2 BC를 전부 0으로 처리
    int      bc_p2_exact_v     = 500;
    int      bc_p2_large_v     = 5000;
    int      bc_p2_fixed_k     = 32;

    int      bc_k_min          = 50;
    uint64_t bc_seed           = 42;

    // ── Complexity score (file-level heatmap) ──────────────────────────────
    // complexity_score = w_cc*pct_cc + w_nesting*pct_nesting + w_loc*pct_loc
    //                   + w_inbound*pct_inbound + w_outbound*pct_outbound
    // Each pct_* is a percentile rank in [0,1] across all scored files.
    double complexity_w_cc       = 0.30;
    double complexity_w_nesting  = 0.20;
    double complexity_w_loc      = 0.15;
    double complexity_w_inbound  = 0.20;
    double complexity_w_outbound = 0.15;

    // cc_score = cc_max_blend*max_cc + cc_avg_blend*avg_cc (pre-blend, before percentile-rank)
    double complexity_cc_max_blend = 0.80;
    double complexity_cc_avg_blend = 0.20;

    // nesting_score = nesting_max_blend*max_depth + nesting_avg_blend*avg_depth
    double complexity_nesting_max_blend = 0.80;
    double complexity_nesting_avg_blend = 0.20;

    // false(0): is_generated=1 files are excluded from the scoring population.
    bool complexity_include_generated = false;

    // ── Graph edge weights ────────────────────────────────────────────────
    // Only PageRank consumes these; SCC and Brandes see the unweighted graph.
    double edge_w_inherits = 3.0;
    double edge_w_calls    = 2.0;
    double edge_w_imports  = 1.0;

    // How the number of entity pairs behind one file→file edge scales its weight.
    //   0 = ignore, 1 = linear (w*cnt), 2 = log (w*(1+ln cnt))
    // Linear lets a single hot file pair swamp the graph, so log is the default.
    int    edge_count_mode = 2;
};
