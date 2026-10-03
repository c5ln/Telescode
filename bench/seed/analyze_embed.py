"""임베딩 seed 진단 — **BM25와 얼마나 독립인가**가 핵심 질문이다.

이 실험에서 임베딩의 목적은 BM25를 이기는 것이 아니다. `RESULTS.md` §7-6:

    PPR seed를 BM25에서 뽑았기 때문에 `ppr`과 `bm25`가 독립이 아니고,
    그래서 ablation에서 graph 그룹의 한계 기여가 유의하지 않았다 (p=0.074).

따라서 성공 기준은 **낮은 `corr(embed, bm25)`**다. 임베딩의 랭킹 성능이
BM25보다 나빠도 목적에는 문제가 없다.

## 상관을 인스턴스 안에서 재는 이유

전체 행을 한 덩어리로 놓고 재면 **인스턴스 간 차이가 상관을 부풀린다.**
어떤 이슈는 후보 전체 점수가 높고 어떤 이슈는 낮은데, 그 공통 변동이 두
신호에 같이 실려 실제보다 강한 상관으로 보인다. 우리가 알고 싶은 것은
"같은 이슈 안에서 두 신호가 파일 순서를 다르게 매기는가"이므로 인스턴스별로
재고 중앙값을 본다. 둘 다 보고한다.

    bench/.venv/bin/python -m bench.seed.analyze_embed \
        --seed bench/data/seed_all/embed_seed__no_paths.csv \
        --features bench/data/features_all_with_bm25__no_paths.csv
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import pandas as pd

from bench.metrics.ranking import evaluate_instance
from bench.schema import C, DATA_DIR, normalize_file_id


def per_instance_corr(df: pd.DataFrame, a: str, b: str, method: str) -> pd.Series:
    """인스턴스별 상관. 한쪽이 상수인 인스턴스는 NaN(정의되지 않음)."""
    out = {}
    for iid, g in df.groupby(C.instance_id):
        if g[a].nunique() < 2 or g[b].nunique() < 2:
            continue
        out[iid] = g[a].corr(g[b], method=method)
    return pd.Series(out, dtype=float)


def rank_metrics(df: pd.DataFrame, score_col: str) -> dict:
    """`ranking.py`의 유일한 구현으로 지표를 낸다. 자체 구현 금지 (CONTRACT §2)."""
    rows = []
    for iid, g in df.groupby(C.instance_id):
        gold = set(g.loc[g[C.is_positive] == 1, C.file_id])
        scanned = set(g[C.file_id])
        if not gold:
            continue
        rows.append(evaluate_instance(
            list(g[C.file_id]), list(g[score_col].fillna(0.0)), gold,
            scanned_file_ids=scanned))
    m = pd.DataFrame(rows)
    return {k: float(np.nanmean(m[k])) for k in
            ["mrr", "recall@10", "ndcg@10", "reachable_recall_ceiling"]}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--seed", type=Path,
                    default=DATA_DIR / "seed_all" / "embed_seed__no_paths.csv")
    ap.add_argument("--features", type=Path,
                    default=DATA_DIR / "features_all_with_bm25__no_paths.csv")
    a = ap.parse_args(argv)

    seed = pd.read_csv(a.seed)
    seed[C.file_id] = seed[C.file_id].map(normalize_file_id)
    feat = pd.read_csv(a.features)
    feat[C.file_id] = feat[C.file_id].map(normalize_file_id)

    cols = [C.instance_id, C.file_id, C.is_positive, C.bm25, C.ppr, C.complexity]
    cols = [c for c in cols if c in feat.columns]
    df = seed.merge(feat[cols], on=[C.instance_id, C.file_id], how="inner")

    print(f"seed {len(seed):,}행 / features {len(feat):,}행 -> 조인 {len(df):,}행")
    print(f"인스턴스 {df[C.instance_id].nunique()}개, "
          f"라벨 결측 {int(df[C.is_positive].isna().sum())}")
    if len(df) < 0.95 * len(seed):
        print("⚠ 조인 손실이 5%를 넘는다. file_id 정규화를 확인하라")

    print("\n── 점수 분포 " + "─" * 50)
    d = df[C.embed]
    print(f"embed  min={d.min():.4f} med={d.median():.4f} max={d.max():.4f} "
          f"고유값={d.nunique():,}/{len(d):,}  0인 행={int((d == 0).sum()):,}")

    print("\n── 판별력 (gold vs 비gold) " + "─" * 36)
    for col in [C.embed, C.bm25]:
        if col not in df.columns:
            continue
        g = df.groupby(C.is_positive)[col].mean()
        if 1 in g.index and 0 in g.index:
            print(f"{col:8s} gold={g[1]:.4f}  비gold={g[0]:.4f}  차이={g[1] - g[0]:+.4f}")

    print("\n── 랭킹 성능 " + "─" * 50)
    for col in [C.embed, C.bm25]:
        if col in df.columns and df[col].notna().any():
            m = rank_metrics(df, col)
            print(f"{col:8s} MRR={m['mrr']:.4f}  R@10={m['recall@10']:.4f}  "
                  f"NDCG@10={m['ndcg@10']:.4f}")

    print("\n── 독립성: corr(embed, bm25)  ← 이 실험의 성공 기준 " + "─" * 12)
    if C.bm25 in df.columns:
        for method in ("pearson", "spearman"):
            pooled = df[C.embed].corr(df[C.bm25], method=method)
            per = per_instance_corr(df, C.embed, C.bm25, method)
            print(f"{method:9s} pooled={pooled:+.4f}   "
                  f"인스턴스별 중앙값={per.median():+.4f}  "
                  f"[{per.quantile(.25):+.3f}, {per.quantile(.75):+.3f}]  n={len(per)}")
        print("\n  pooled가 인스턴스별보다 높으면 그 차이는 인스턴스 간 변동이다")
        print("  (모듈 docstring 참조). 판단은 인스턴스별 중앙값으로 한다.")

    if C.ppr in df.columns and df[C.ppr].notna().any():
        print("\n── 참고: 기존 ppr(=BM25 seed)와의 관계 " + "─" * 24)
        per = per_instance_corr(df, C.embed, C.ppr, "spearman")
        print(f"corr(embed, ppr) 인스턴스별 중앙값={per.median():+.4f}  n={len(per)}")


if __name__ == "__main__":
    main()
