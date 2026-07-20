#pragma once

#include <cstdint>

// 읽기 순서 알고리즘 전체에 걸친 하이퍼파라미터 집합.
// DB 동기화는 AlgoDbWriter::loadConfig / write 를 통해 이루어진다.
struct AlgoConfig {
    // combined = (alpha/(alpha+beta)) * PR_norm + (beta/(alpha+beta)) * BC_norm
    double   alpha             = 0.6;
    double   beta              = 0.4;

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
};
