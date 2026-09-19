"""Phase 4 실행기 — baseline · LTR · 그룹 ablation · 크기 통제.

    bench/.venv/bin/python -m bench.ltr.run_ltr \
        --features bench/data/features.csv \
        --manifest bench/data/instances.csv \
        --bm25-seed bench/data/bm25_seed__full.csv \
        --overlap bench/data/vocab_overlap.csv \
        --out-prefix bench/data/ltr

## 이 스크립트가 지키는 세 가지

1. **비교 기준선은 `complexity` 단독이다.** B2(0.6·PR+0.4·BC)가 아니다.
   `complexity`가 전체 MRR에서 모든 그래프 baseline을 크게 이기므로, "LTR이 B2를
   이겼다"는 넘기 쉬운 선을 골라 놓고 이겼다는 말이다. 표의 헤드라인 비교는
   `LTR vs complexity`로 고정한다.

2. **ablation은 전체와 non_overlap을 각각 낸다.** 전체 평균만 내면 두 신호가
   서로 다른 구간에서 작동한다는 사실이 상쇄되어 사라진다. "graph를 빼도 전체는
   그대로인데 non_overlap이 무너진다"가 나오면 그게 구조 신호의 존재 이유다.

3. **모든 부분집합 수치에 n을 붙인다.** `n_instances`와 `n_gold`가 없는 칸은
   이 스크립트가 만들지 않는다. n<20 구간에는 부트스트랩 구간과 `underpowered`
   표시를 함께 낸다.

ablation 비교는 **같은 fold 배정**을 공유한다. fold가 다르면 차이가 피처 때문인지
분할 때문인지 구분할 수 없다.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import pandas as pd

from bench.features.code_lexical import CX, EXTRA_GROUPS
from bench.ltr import cv as M
from bench.ltr.data import FEATURE_GROUPS, load_dataset, restrict_candidates
from bench.ltr.evaluate import (evaluate_all, fmt, paired_bootstrap,
                                per_instance_metrics)
from bench.ltr.size_control import (correlation_report, residual_ranking_report,
                                    size_matched_comparison, size_stratified)
from bench.metrics.baseline import B2_ALPHA, B2_BETA, make_splits, minmax
from bench.schema import C, DATA_DIR

# (라벨, 컬럼, 클수록 좋은가). `bench.metrics.baseline.BASELINES`와 같은 목록에
# BM25를 더한 것이다. 그쪽은 bm25가 없던 시점에 쓰였다.
SINGLE_BASELINES = [
    ("embed (Qwen3 임베딩)", C.embed, True),
    ("rerank (크로스 인코더)", C.rerank, True),
    ("bm25", C.bm25, True),
    ("def_match (정의 이름)", CX.def_match, True),
    ("str_match (문자열 리터럴)", CX.str_match, True),
    ("nbr_bm25_max (이웃 BM25)", CX.nbr_bm25_max, True),
    ("ppr (목적 조건부 전파)", C.ppr, True),
    ("complexity", C.complexity, True),
    ("logical_loc", C.logical_loc, True),
    ("out_deg", C.out_deg, True),
    ("in_deg", C.in_deg, True),
    ("bc", C.bc, True),
    ("pagerank", C.pagerank, True),
    ("combined (제품 현재 설정)", C.combined, True),
    ("file_rank (제품 현재 출력)", C.file_rank, False),
]

B2_COL = "__b2__"
BASE_LABEL = "base (추가 피처 제외)"


def add_b2(features: pd.DataFrame) -> pd.DataFrame:
    """B2 = 0.6·norm(pagerank) + 0.4·norm(bc). 인스턴스 안에서 정규화한다.

    `bench.metrics.baseline.b2_score`와 같은 정의이고 계수도 그 모듈에서 가져온다.
    여기서 숫자를 다시 적으면 한쪽만 바뀌었을 때 두 표가 조용히 갈린다.
    """
    df = features.copy()
    df[B2_COL] = np.nan
    for _, g in df.groupby(C.instance_id, sort=False):
        df.loc[g.index, B2_COL] = (
            B2_ALPHA * minmax(g[C.pagerank].to_numpy(dtype=float))
            + B2_BETA * minmax(g[C.bc].to_numpy(dtype=float)))
    return df


def run_baselines(ds, features: pd.DataFrame) -> tuple[pd.DataFrame, dict]:
    rows, per = [], {}
    entries = list(SINGLE_BASELINES) + [("B2 = 0.6·PR + 0.4·BC", B2_COL, True)]
    for label, col, hib in entries:
        if col not in features.columns or features[col].isna().all():
            continue
        sc = features[[C.instance_id, C.file_id, col]]
        r, p = evaluate_all(ds, sc, col, label, higher_is_better=hib)
        rows.extend(r)
        per[label] = p
    return pd.DataFrame(rows), per


def ablation_configs(ds, base_extra_groups: tuple[str, ...] = (),
                     extra_only: bool = False) -> list[tuple[str, list[str]]]:
    """제거형(leave-one-group-out)·단독형(group-only)과 **PPR 분리 조건**을 만든다.

    제거형만 보면 그룹이 중복된 정보를 담을 때 둘 다 "빼도 상관없다"로 나온다.
    단독형을 같이 보면 그 그룹 하나로 어디까지 가는지 알 수 있다.

    ## graph 그룹을 통째로 다루면 안 되는 이유

    graph 그룹 안에 성격이 전혀 다른 두 종류가 섞여 있다.

      - `pagerank`, `bc`, `in_deg`, `out_deg`, `combined`, `file_rank`
        → **전역** 중요도. 이슈가 무엇이든 값이 같다.
      - `ppr`
        → **목적 조건부** 전파. 이슈에서 출발한 teleport 분포로 개인화된다.

    이 프로젝트의 주장은 "전역 중요도가 유용하다"가 아니라 "목적에서 출발한 구조
    전파가 유용하다"이다. 그래서 그룹 단위 ablation과 **별개로** 다음 둘을 만든다.

      - `-ppr`             : 나머지 전부 두고 ppr만 뺀다. 전역 지표 위에 목적
                             조건부 전파가 **추가로** 기여하는지를 직접 잰다.
      - `ppr only`         : ppr 하나로 어디까지 가는지.
      - `graph global only`: ppr을 뺀 graph. `ppr only`와 짝지어 읽으면 같은
                             그래프에서 나온 두 신호의 격차가 그대로 드러난다.
    """
    groups = {g: ds.groups_for(g) for g in FEATURE_GROUPS}
    groups = {g: c for g, c in groups.items() if c}
    # `--extra-features`로 붙인 코드 어휘 피처. 없으면 아래는 전부 이전과 같다.
    # `base_extra_groups`에 든 것은 base에 접어 넣는다 — 이전 실험의 full을
    # 새 기준선으로 삼고 그 위에서 신규 그룹만 재기 위해서다.
    extras = {g: ds.groups_for(g) for g in EXTRA_GROUPS}
    extras = {g: c for g, c in extras.items() if c}
    base_extra = [c for g, v in extras.items() if g in base_extra_groups for c in v]
    extras = {g: c for g, c in extras.items() if g not in base_extra_groups}
    extra_cols = [c for v in extras.values() for c in v]
    core = [c for g in groups.values() for c in g]
    base = core + base_extra
    allc = base + extra_cols

    cfgs = [("full (all groups)", allc)]
    if not extra_only:
        for g in groups:
            rest = ([c for k, v in groups.items() if k != g for c in v]
                    + base_extra + extra_cols)
            if rest:
                cfgs.append((f"-{g}", rest))
        for g, cols in groups.items():
            cfgs.append((f"{g} only", cols))

    # 신규 피처는 두 방향으로 잰다: base에 하나만 더했을 때(단독 기여)와
    # full에서 하나만 뺐을 때(한계 기여). 서로 겹치면 둘이 다르게 나온다.
    if extras:
        cfgs.append((BASE_LABEL, base))
        for g, cols in extras.items():
            cfgs.append((f"base +{g}", base + cols))
            # 그룹이 둘이면 `-A`는 `base +B`와 같은 피처 구성이다 — 중복 학습을 건너뛴다.
            if len(extras) > 2:
                cfgs.append((f"-{g}", [c for c in allc if c not in cols]))

    if extra_only:
        return cfgs
    if C.ppr in ds.feature_columns:
        cfgs.append(("-ppr (목적 조건부만 제거)",
                     [c for c in allc if c != C.ppr]))
        cfgs.append(("ppr only", [C.ppr]))
        gg = [c for c in groups.get("graph", []) if c != C.ppr]
        if gg:
            cfgs.append(("graph global only (ppr 제외)", gg))
    return cfgs


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--features", default=str(DATA_DIR / "features.csv"))
    ap.add_argument("--manifest", default=str(DATA_DIR / "instances.csv"))
    ap.add_argument("--bm25-seed", default=str(DATA_DIR / "bm25_seed__full.csv"),
                    help="재스캔 매트릭스의 bm25가 비어 있을 때 조인할 seed")
    ap.add_argument("--embed-seed", default=None,
                    help="임베딩 seed CSV. 미지정 시 embed/embed_rank는 NaN으로 남는다")
    ap.add_argument("--rerank-seed", default=None,
                    help="리랭킹 seed CSV. 후보 밖 행은 NaN으로 남는다")
    ap.add_argument("--extra-features", default=None,
                    help="bench.features.code_lexical 산출 CSV. 주면 base/+X/-X ablation을 추가한다")
    ap.add_argument("--base-extra-groups", default="",
                    help="base에 접어 넣을 extra 그룹 (쉼표). 예: def_match,str_match,nbr_bm25")
    ap.add_argument("--extra-only", action="store_true",
                    help="graph/complexity/semantic/ppr ablation을 건너뛰고 extra 그룹만 잰다")
    ap.add_argument("--candidate-top-k", type=int, default=0,
                    help="기본 0은 전체 후보. 양수면 BM25/PPR+RRF로 top-k 제한")
    ap.add_argument("--include-init", action="store_true",
                    help="기본적으로 제외하는 __init__.py를 후보에 포함")
    ap.add_argument("--overlap", default=str(DATA_DIR / "vocab_overlap.csv"))
    ap.add_argument("--overlap-condition", default="full")
    ap.add_argument("--splits", default=str(DATA_DIR / "splits.csv"))
    ap.add_argument("--out-prefix", default=str(DATA_DIR / "ltr"))
    ap.add_argument("--exclude-generated", action="store_true")
    ap.add_argument("--no-tune", action="store_true",
                    help="inner CV를 건너뛰고 PARAM_GRID[0] 고정 (빠른 확인용)")
    ap.add_argument("--no-normalize", action="store_true",
                    help="인스턴스 내 min-max 정규화 없이 원값으로 학습 (강건성 확인)")
    ap.add_argument("--skip-holdout", action="store_true")
    args = ap.parse_args(argv)

    ds = load_dataset(Path(args.features), Path(args.manifest),
                      bm25_seed_path=Path(args.bm25_seed) if args.bm25_seed else None,
                      embed_seed_path=Path(args.embed_seed) if args.embed_seed else None,
                      rerank_seed_path=Path(args.rerank_seed) if args.rerank_seed else None,
                      extra_features_path=(Path(args.extra_features)
                                           if args.extra_features else None),
                      overlap_path=Path(args.overlap) if args.overlap else None,
                      overlap_condition=args.overlap_condition,
                      exclude_generated=args.exclude_generated)
    if args.candidate_top_k < 0:
        raise SystemExit("--candidate-top-k는 0 이상이어야 한다")
    ds = restrict_candidates(ds, args.candidate_top_k or None,
                             exclude_init=not args.include_init)

    print("=" * 78)
    print("입력")
    print("=" * 78)
    print(f"  features : {args.features}")
    print(f"  manifest : {args.manifest}")
    print(f"  행 {len(ds.features)} · 인스턴스 {len(ds.instance_ids)} · "
          f"positive 행 {int(ds.features[C.is_positive].sum())}")
    print(f"  사용 피처 {len(ds.feature_columns)}개: {', '.join(ds.feature_columns)}")
    for n in ds.notes:
        print(f"  - {n}")
    for seg in ("overlap", "non_overlap"):
        print(f"  구간 {seg}: 인스턴스 {ds.segment_instance_count(seg)} · "
              f"gold {ds.segment_gold_count(seg)}")
    print()

    features = add_b2(ds.features)
    ds.features = features

    # ── 1. 단일 피처 baseline ────────────────────────────────────────────
    base_tbl, base_per = run_baselines(ds, features)
    print("=" * 78)
    print("1. 단일 피처 baseline (전체 / 구간별)")
    print("=" * 78)
    for seg in ("all", "overlap", "non_overlap"):
        sub = base_tbl[base_tbl["segment"] == seg]
        if sub.empty:
            continue
        print(f"\n── segment = {seg} ──")
        print(fmt(sub.drop(columns=["segment"]).sort_values("MRR", ascending=False)))
    print()

    # ── 2. LTR + ablation ────────────────────────────────────────────────
    folds = M.make_folds(ds.instance_ids, M.N_OUTER_FOLDS, M.SEED)
    base_extra = tuple(g for g in args.base_extra_groups.split(",") if g)
    unknown = set(base_extra) - set(EXTRA_GROUPS)
    if unknown:
        raise SystemExit(f"알 수 없는 extra 그룹: {sorted(unknown)}")
    cfgs = ablation_configs(ds, base_extra, args.extra_only)
    ltr_rows, ltr_per, gains, chosen, ltr_scores = [], {}, {}, {}, {}

    print("=" * 78)
    print(f"2. LambdaRank OOF ({folds.n_folds}-fold, nested inner CV"
          f"{'' if not args.no_tune else ' — 튜닝 생략'})")
    print("=" * 78)
    for label, cols in cfgs:
        res = M.oof_predict(ds, cols, normalize=not args.no_normalize,
                            folds=folds, tune=not args.no_tune)
        r, p = evaluate_all(ds, res.scores, "ltr_score", f"LTR {label}")
        ltr_rows.extend(r)
        ltr_per[label] = p
        ltr_scores[label] = res.scores
        gains[label] = res.gain
        chosen[label] = res.chosen_params
        print(f"  {label:22s} 피처 {len(cols):2d}개  "
              f"inner NDCG@10 {np.nanmean(res.inner_scores):.4f}")
    ltr_tbl = pd.DataFrame(ltr_rows)
    print()
    for seg in ("all", "overlap", "non_overlap"):
        sub = ltr_tbl[ltr_tbl["segment"] == seg]
        if sub.empty:
            continue
        print(f"── segment = {seg} ──")
        print(fmt(sub.drop(columns=["segment"])))
        print()

    print("── full 모델 피처 gain 비중 ──")
    print(fmt(gains["full (all groups)"].reset_index().rename(columns={"index": "feature"})))
    print()

    # ── 3. 헤드라인 비교: LTR vs complexity (B2 아님) ─────────────────────
    print("=" * 78)
    print("3. 헤드라인 비교 — 기준선은 complexity 단독이다 (B2 아님)")
    print("=" * 78)
    tests = {}
    full_per = ltr_per["full (all groups)"]
    for seg in ("all", "overlap", "non_overlap"):
        if seg not in full_per or full_per[seg].empty:
            continue
        for ref in ("complexity", "bm25", "ppr (목적 조건부 전파)",
                    "B2 = 0.6·PR + 0.4·BC"):
            if ref not in base_per or seg not in base_per[ref]:
                continue
            t = paired_bootstrap(full_per[seg], base_per[ref][seg])
            tests[f"LTR(full) - {ref} [{seg}]"] = t
    _print_tests(tests)

    # ── 3b. 목적 조건부 전파 vs 전역 지표 ────────────────────────────────
    if "ppr (목적 조건부 전파)" in base_per:
        print("=" * 78)
        print("3b. ppr(목적 조건부) vs 전역 그래프 지표 — 같은 그래프, 다른 질문")
        print("=" * 78)
        print("  둘 다 같은 링크 그래프에서 나온다. 차이는 출발점뿐이다:")
        print("  ppr은 이슈에서 teleport하고, pagerank/bc는 이슈를 안 본다.\n")
        ppr_tests = {}
        for seg in ("all", "overlap", "non_overlap"):
            cur = base_per["ppr (목적 조건부 전파)"].get(seg)
            if cur is None or cur.empty:
                continue
            for ref in ("pagerank", "bc", "B2 = 0.6·PR + 0.4·BC",
                        "combined (제품 현재 설정)", "complexity", "bm25"):
                r = base_per.get(ref, {}).get(seg)
                if r is None or r.empty:
                    continue
                ppr_tests[f"ppr - {ref} [{seg}]"] = paired_bootstrap(cur, r)
        _print_tests(ppr_tests)
        tests.update(ppr_tests)

    # ── 4. ablation: 그룹 제거 효과, 전체 vs non_overlap ──────────────────
    print("=" * 78)
    print("4. Ablation — 그룹 제거 효과 (구간별)")
    print("=" * 78)
    ab_tests = {}
    for label in [l for l, _ in cfgs if l != "full (all groups)"]:
        for seg in ("all", "overlap", "non_overlap"):
            if seg not in full_per or seg not in ltr_per[label]:
                continue
            if full_per[seg].empty or ltr_per[label][seg].empty:
                continue
            ab_tests[f"{label} - full [{seg}]"] = paired_bootstrap(
                ltr_per[label][seg], full_per[seg])
    if BASE_LABEL in ltr_per:
        # 신규 피처의 단독 기여: base 대비. full도 같이 — "셋 다 넣으면 얼마나 오르나".
        base_per = ltr_per[BASE_LABEL]
        for label in [l for l, _ in cfgs if l.startswith("base +")] + ["full (all groups)"]:
            for seg in ("all", "overlap", "non_overlap"):
                cur, ref = ltr_per[label].get(seg), base_per.get(seg)
                if cur is None or ref is None or cur.empty or ref.empty:
                    continue
                ab_tests[f"{label} - base [{seg}]"] = paired_bootstrap(cur, ref)
    _print_tests(ab_tests)

    # ── 4b. 학습 없는 hybrid 결합 (RRF) ──────────────────────────────────
    print("=" * 78)
    print("4b. Reciprocal Rank Fusion — 전역 LTR이 못 쓴 결합을 학습 없이 확인")
    print("=" * 78)
    fusion_tbl, fusion_tests = _run_fusion(
        ds, features, ltr_scores["full (all groups)"], base_per, full_per)
    for seg in ("all", "overlap", "non_overlap"):
        sub = fusion_tbl[fusion_tbl["segment"] == seg]
        if sub.empty:
            continue
        print(f"── segment = {seg} ──")
        print(fmt(sub.drop(columns=["segment"])))
        print()
    _print_tests(fusion_tests)

    # ── 5. 크기 통제 ─────────────────────────────────────────────────────
    print("=" * 78)
    print("5. complexity 크기 통제 (Kochhar ASE 2014 편향)")
    print("=" * 78)
    print("── complexity vs logical_loc 인스턴스별 상관 ──")
    print(fmt(correlation_report(features)))
    print("\n── 잔차 랭킹 ──")
    resid_tbl, resid_tests = residual_ranking_report(ds)
    for seg in ("all", "overlap", "non_overlap"):
        sub = resid_tbl[resid_tbl["segment"] == seg]
        if sub.empty:
            continue
        print(f"  segment = {seg}")
        print(fmt(sub.drop(columns=["segment"])))
    print()
    _print_tests(resid_tests)
    print("── 크기 매칭 짝 비교 (gold ↔ 최근접 logical_loc 비gold) ──")
    pairs, pair_summary = size_matched_comparison(features)
    print(fmt(pair_summary) if not pair_summary.empty else "  짝을 만들 수 없다")
    print("\n── logical_loc 사분위 구간별 ──")
    print(fmt(size_stratified(features)))
    print()

    # ── 6. 부차 확인: pin된 holdout 분할 ─────────────────────────────────
    holdout_tbl = pd.DataFrame()
    if not args.skip_holdout and Path(args.splits).exists():
        print("=" * 78)
        print("6. 부차 확인 — pin된 splits.csv holdout (OOF와 크게 어긋나면 CV를 의심)")
        print("=" * 78)
        splits = pd.read_csv(args.splits)
        # pin된 splits.csv는 xarray 110개 시점에 만들어졌다. 통합 매트릭스로 돌리면
        # 나머지 인스턴스가 train/test 어느 쪽에도 안 들어가 조용히 사라진다.
        # 덮이지 않으면 같은 해시 규칙으로 다시 만든다 — 기존 배정은 안 흔들린다.
        uncovered = set(ds.instance_ids) - set(splits[C.instance_id])
        if uncovered:
            print(f"  splits.csv가 인스턴스 {len(uncovered)}개를 안 덮는다 — "
                  f"make_splits로 재생성 (해시 기반이라 기존 배정 유지)")
            splits = make_splits(ds.instance_ids)
        hs = M.holdout_predict(ds, cfgs[0][1], splits,
                               normalize=not args.no_normalize, tune=not args.no_tune)
        rows, _ = evaluate_all(ds, hs, "ltr_score", "LTR full (holdout)")
        holdout_tbl = pd.DataFrame(rows)
        print(fmt(holdout_tbl))
        print()

    # ── 산출 ─────────────────────────────────────────────────────────────
    pref = args.out_prefix
    base_tbl.to_csv(f"{pref}_baselines.csv", index=False)
    ltr_tbl.to_csv(f"{pref}_ablation.csv", index=False)
    resid_tbl.to_csv(f"{pref}_size_control.csv", index=False)
    fusion_tbl.to_csv(f"{pref}_fusion.csv", index=False)
    if not pairs.empty:
        pairs.to_csv(f"{pref}_size_pairs.csv", index=False)
    if not holdout_tbl.empty:
        holdout_tbl.to_csv(f"{pref}_holdout.csv", index=False)
    gains["full (all groups)"].to_csv(f"{pref}_gain.csv")
    with open(f"{pref}_tests.json", "w") as fh:
        json.dump({"headline": tests, "ablation": ab_tests,
                   "fusion": fusion_tests, "size_control": resid_tests,
                   "chosen_params": {k: v for k, v in chosen.items()},
                   "notes": ds.notes,
                   "dropped_features": ds.dropped_columns},
                  fh, ensure_ascii=False, indent=2)
    print(f"산출 → {pref}_baselines.csv, {pref}_ablation.csv, "
          f"{pref}_size_control.csv, {pref}_gain.csv, {pref}_tests.json")
    return 0


def _run_fusion(ds, features: pd.DataFrame, ltr_scores: pd.DataFrame,
                base_per: dict, full_per: dict) -> tuple[pd.DataFrame, dict]:
    """RRF 조합 몇 가지를 같은 지표·같은 구간으로 평가한다.

    조합은 **가설이 지목하는 것만** 넣는다. 조합을 많이 만들어 놓고 non_overlap에서
    제일 높은 것을 고르면 그건 n=16짜리 구간에 대한 선택 편의다. 여기 있는 넷은
    전부 "어휘 랭커 + 구조 랭커"라는 하나의 가설에서 나온다.
    """
    from bench.ltr.fusion import RRF_K, rrf, with_external

    f = with_external(features, ltr_scores, "ltr_score")

    combos = [
        ("RRF(bm25, ppr)", [(C.bm25, True), (C.ppr, True)]),
        ("RRF(bm25, bc)", [(C.bm25, True), (C.bc, True)]),
        ("RRF(bm25, combined)", [(C.bm25, True), (C.combined, True)]),
        ("RRF(bm25, complexity, bc)",
         [(C.bm25, True), (C.complexity, True), (C.bc, True)]),
        ("RRF(LTR, ppr)", [("ltr_score", True), (C.ppr, True)]),
        ("RRF(LTR, bc)", [("ltr_score", True), (C.bc, True)]),
        ("RRF(LTR, combined)", [("ltr_score", True), (C.combined, True)]),
    ]
    combos = [(lab, r) for lab, r in combos
              if all(c in f.columns and not f[c].isna().all() for c, _ in r)]

    rows, per = [], {}
    for label, rankers in combos:
        sc = rrf(f, rankers, k=RRF_K)
        r, p = evaluate_all(ds, sc, "rrf", label)
        rows.extend(r)
        per[label] = p

    tests = {}
    for seg in ("all", "non_overlap"):
        for label in per:
            if seg not in per[label] or seg not in full_per:
                continue
            if per[label][seg].empty or full_per[seg].empty:
                continue
            tests[f"{label} - LTR(full) [{seg}]"] = paired_bootstrap(
                per[label][seg], full_per[seg])
        ref = base_per.get("bm25", {}).get(seg)
        if ref is not None and not ref.empty:
            for label in per:
                if seg in per[label] and not per[label][seg].empty:
                    tests[f"{label} - bm25 [{seg}]"] = paired_bootstrap(
                        per[label][seg], ref)
    return pd.DataFrame(rows), tests


def _print_tests(tests: dict) -> None:
    """짝지은 부트스트랩 결과. n<20이면 `underpowered`를 그대로 찍는다."""
    if not tests:
        print("  (비교할 짝이 없다)")
        return
    rows = []
    for k, v in tests.items():
        if not v.get("n_paired"):
            rows.append({"비교": k, "n": 0, "ΔMRR": np.nan})
            continue
        rows.append({"비교": k, "n": v["n_paired"], "ΔMRR": v["diff"],
                     "95% CI": f"[{v['ci_lo']:+.4f}, {v['ci_hi']:+.4f}]",
                     "p": v["p_two_sided"],
                     "underpowered": "YES" if v["underpowered"] else ""})
    print(pd.DataFrame(rows).to_string(
        index=False, float_format=lambda v: f"{v:+.4f}"))
    print()


if __name__ == "__main__":
    raise SystemExit(main())
