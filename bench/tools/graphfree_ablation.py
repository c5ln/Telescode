"""그래프가 전혀 들어가지 않는 `-graph` ablation (개편 후 재측정, 감사 후속 A).

## 왜 필요한가

`run_ltr`의 `-graph` 구성은 graph 그룹을 빼지만 `complexity`(=`file.complexity_score`)는
남긴다. 그런데 `complexity_score`는 그래프 차수를 섞는다 (`src/algo/Scoring.cpp`
`ComplexityScorer::compute`, 가중치 `src/algo/AlgoConfig.h`):

    complexity = 0.30·pct(cc) + 0.20·pct(nesting) + 0.15·pct(loc)      ← 차수 없음
               + 0.20·pct(inbound) + 0.15·pct(outbound)               ← 파일 그래프 차수
    cc      = 0.8·max_cc    + 0.2·avg_cc
    nesting = 0.8·max_depth + 0.2·avg_depth
    pct(v)  = (v보다 작은 값의 개수) / (N−1)   N=1이면 0, 전부 같으면 0.5

pct는 **인스턴스 안에서, `is_generated == 0`인 파일만** 모아 계산한다
(`complexity_include_generated = false`). 생성 파일은 채점 대상이 아니라
`complexity_score`가 스키마 기본값 0.0으로 남는다.

그래서 `-graph`는 차수 정보를 `complexity`로 계속 본다. 여기서는 앞의 세 항만 다시
계산한 `complexity_nodeg`로 바꿔 **그래프가 전혀 없는** `-graph`를 만든다.

## 생성 파일 처리

`is_generated == 1`(99행 · 53 인스턴스, xarray의 `_typed_ops.py` 등 3개 파일)은
`complexity_nodeg = 0.0`으로 둔다. 바이너리가 그 행을 채점하지 않아 `complexity`가
0.0인 것과 같은 규약이다. 0.0은 상수라 그래프 정보가 없다. 백분위의 모집단에서도
뺀다(Scoring.cpp와 같다). 생성 파일만 따로 채점하면 바이너리에 없는 값이 생긴다.

## 검증 (계산이 틀리면 결론이 틀린다)

1. 생성 파일이 아닌 모든 행에서
   `complexity == complexity_nodeg + 0.20·pct(in_deg) + 0.15·pct(out_deg)`인지 본다.
   in_deg/out_deg가 Graph.cpp의 radj/adj 크기와 같다는 가정도 함께 확인된다.
2. `--old-features`를 주면 옛 그래프 매트릭스의 `complexity_nodeg`가 새 것과 같은지 본다.
   같으면 graph-free `-graph`의 입력은 두 그래프에서 같다.

## 무엇을 돌리나 (run_ltr과 같은 fold·nested CV·전처리, `__init__.py` 포함)

    full                         : run_ltr과 같은 19개 피처 (재현 확인용)
    -graph (run_ltr)             : complexity(차수 포함) + 복잡도 원시 5개 + semantic
    -graph (graph-free)          : complexity_nodeg + 복잡도 원시 5개 + semantic
    full (complexity_nodeg)      : 그래프는 graph 그룹으로만 들어간다

    bench/.venv/bin/python -m bench.tools.graphfree_ablation \
        --features bench/data/resolver_fix/features_all_embedppr.csv \
        --manifest bench/data/resolver_fix/instances_all_embedppr.csv \
        --bm25-seed bench/data/seed_all/bm25_seed__no_paths.csv \
        --embed-seed bench/data/seed_all/embed_seed__no_paths.csv \
        --rerank-seed bench/data/seed_all/rerank_seed__no_paths.csv \
        --overlap bench/data/vocab_overlap.csv --overlap-condition no_paths \
        --old-features bench/data/features_all_embedppr.csv \
        --out bench/data/resolver_fix/graphfree_rerank.json
"""

from __future__ import annotations

import argparse
import dataclasses
import json
from pathlib import Path

import numpy as np
import pandas as pd

from bench.ltr import cv as M
from bench.ltr.data import FEATURE_GROUPS, load_dataset, restrict_candidates
from bench.ltr.evaluate import evaluate_all, paired_bootstrap
from bench.schema import C

# src/algo/AlgoConfig.h (합이 1.0이라 normalizeWeights가 손대지 않는다)
W_CC, W_NEST, W_LOC, W_IN, W_OUT = 0.30, 0.20, 0.15, 0.20, 0.15
CC_MAX, CC_AVG = 0.80, 0.20
NEST_MAX, NEST_AVG = 0.80, 0.20

NODEG = "complexity_nodeg"


def percentile_rank(values: np.ndarray) -> np.ndarray:
    """`Scoring.cpp` `percentile_rank_normalize`와 같은 규칙."""
    n = len(values)
    if n == 0:
        return np.array([])
    if n == 1:
        return np.zeros(1)
    s = np.sort(values)
    if s[0] == s[-1]:
        return np.full(n, 0.5)
    return np.searchsorted(s, values, side="left") / (n - 1)


def add_nodeg(features: pd.DataFrame) -> tuple[pd.DataFrame, dict]:
    """`complexity_nodeg`를 붙이고 분해 항등식을 검증한다."""
    df = features.copy()
    df[NODEG] = 0.0
    resid = np.zeros(len(df))
    for _, g in df.groupby(C.instance_id, sort=False):
        g = g[g[C.is_generated] == 0]
        if g.empty:
            continue
        cc = CC_MAX * g[C.max_cc].to_numpy(float) + CC_AVG * g[C.avg_cc].to_numpy(float)
        nest = (NEST_MAX * g[C.max_depth].to_numpy(float)
                + NEST_AVG * g[C.avg_depth].to_numpy(float))
        loc = g[C.logical_loc].to_numpy(float)
        pin = percentile_rank(g[C.in_deg].to_numpy(float))
        pout = percentile_rank(g[C.out_deg].to_numpy(float))
        nodeg = W_CC * percentile_rank(cc) + W_NEST * percentile_rank(nest) \
            + W_LOC * percentile_rank(loc)
        # C++와 같은 덧셈 순서로 다시 합쳐 complexity와 비교한다
        full = nodeg + W_IN * pin + W_OUT * pout
        df.loc[g.index, NODEG] = nodeg
        resid[df.index.get_indexer(g.index)] = full - g[C.complexity].to_numpy(float)
    gen = df[C.is_generated] == 1
    check = {
        "n_rows": int(len(df)),
        "n_generated_rows": int(gen.sum()),
        "generated_complexity_all_zero": bool((df.loc[gen, C.complexity] == 0).all()),
        "max_abs_identity_residual": float(np.abs(resid[~gen.to_numpy()]).max()),
    }
    return df, check


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--features", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--bm25-seed", required=True)
    ap.add_argument("--embed-seed", default=None)
    ap.add_argument("--rerank-seed", default=None)
    ap.add_argument("--overlap", default=None)
    ap.add_argument("--overlap-condition", default="no_paths")
    ap.add_argument("--old-features", default=None,
                    help="옛 그래프 매트릭스. complexity_nodeg 동일성 확인용")
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)

    p = lambda v: Path(v) if v else None  # noqa: E731
    ds = load_dataset(Path(a.features), Path(a.manifest), bm25_seed_path=p(a.bm25_seed),
                      embed_seed_path=p(a.embed_seed), rerank_seed_path=p(a.rerank_seed),
                      overlap_path=p(a.overlap), overlap_condition=a.overlap_condition)
    ds = restrict_candidates(ds, None, exclude_init=False)    # = run_ltr --include-init

    feats, check = add_nodeg(ds.features)
    if check["max_abs_identity_residual"] > 1e-12 or not check["generated_complexity_all_zero"]:
        raise SystemExit(f"complexity 분해가 맞지 않는다: {check}")
    report = {"inputs": vars(a), "identity_check": check}

    if a.old_features:
        old, old_check = add_nodeg(pd.read_csv(a.old_features))
        m = feats[[C.instance_id, C.file_id, NODEG]].merge(
            old[[C.instance_id, C.file_id, NODEG]], on=[C.instance_id, C.file_id],
            suffixes=("_new", "_old"))
        report["old_identity_check"] = old_check
        report["nodeg_old_vs_new"] = {
            "n_rows_joined": int(len(m)),
            "max_abs_diff": float((m[NODEG + "_new"] - m[NODEG + "_old"]).abs().max())}

    groups = {g: ds.groups_for(g) for g in FEATURE_GROUPS}
    full = [c for v in groups.values() for c in v]
    minus_graph = [c for g, v in groups.items() if g != "graph" for c in v]
    swap = lambda cols: [NODEG if c == C.complexity else c for c in cols]  # noqa: E731
    if C.complexity not in minus_graph:
        raise SystemExit("complexity가 피처에 없다")

    ds_n = dataclasses.replace(ds, features=feats)
    cfgs = [("full", full), ("-graph (run_ltr)", minus_graph),
            ("-graph (graph-free)", swap(minus_graph)),
            ("full (complexity_nodeg)", swap(full))]
    folds = M.make_folds(ds_n.instance_ids, M.N_OUTER_FOLDS, M.SEED)
    per, rows = {}, []
    for label, cols in cfgs:
        res = M.oof_predict(ds_n, cols, normalize=True, folds=folds, tune=True)
        r, pi = evaluate_all(ds_n, res.scores, "ltr_score", label)
        rows.extend(r)
        per[label] = pi
        print(f"{label:26s} MRR {pi['all']['mrr'].mean():.4f}", flush=True)
    report["metrics"] = rows

    tests = {}
    for seg in ("all", "non_overlap"):
        for lab in ("-graph (run_ltr)", "-graph (graph-free)", "full (complexity_nodeg)"):
            if seg in per[lab] and not per[lab][seg].empty:
                tests[f"{lab} - full [{seg}]"] = paired_bootstrap(per[lab][seg], per["full"][seg])
        if seg in per["full"] and not per["full"][seg].empty:
            tests[f"-graph (graph-free) - -graph (run_ltr) [{seg}]"] = paired_bootstrap(
                per["-graph (graph-free)"][seg], per["-graph (run_ltr)"][seg])
    report["tests"] = tests
    for k, v in tests.items():
        print(f"  {k:52s} {v['diff']:+.4f} [{v['ci_lo']:+.4f}, {v['ci_hi']:+.4f}] "
              f"p={v['p_two_sided']:.3f} n={v['n_paired']}")
    Path(a.out).write_text(json.dumps(report, indent=2, ensure_ascii=False, default=float))
    print(f"→ {a.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
