"""PPR 동작 진단 — RESULTS.md §5 "PPR = PageRank 순서 + seed 파일 하나를 맨 앞으로".

§5의 수치(인스턴스별 스피어만 0.945/0.933, seed 파일이 PPR 1위 88.2%, 두 PPR의 순위
상관 0.979, BM25·embed top-1 일치 17.5%)는 대화형으로 계산돼 코드가 남아 있지
않았다. 6단계에서 그래프를 바꾼 뒤 같은 수치를 다시 내려고 만든 것이다. 같은
스크립트를 옛 매트릭스에도 돌려 옛 수치를 재현하는지부터 확인한다.

라벨(`is_positive`)은 읽지 않는다 — 순위끼리의 관계만 본다.

    bench/.venv/bin/python -m bench.tools.ppr_mechanism \
        --embed-ppr bench/data/features_all_embedppr.csv \
        --bm25-ppr  bench/data/features_all.csv \
        --embed-seed bench/data/seed_all/embed_seed__no_paths.csv \
        --bm25-seed  bench/data/seed_all/bm25_seed__no_paths.csv \
        --seed-topk 1 --seed-power 1 --out <json>
"""

from __future__ import annotations

import argparse
import json

import numpy as np
import pandas as pd
from scipy.stats import pearsonr, spearmanr

from bench.collect.pipeline import sharpen_seed
from bench.schema import C


def _repo(iid: str) -> str:
    return iid.split("__", 1)[1].rsplit("-", 1)[0]


def _rank_desc(s: pd.Series) -> pd.Series:
    """1 = 최고. 동점은 비관적(최악 순위) — 평가 규약과 같은 방향."""
    return s.rank(ascending=False, method="max")


def ppr_vs_pagerank(df: pd.DataFrame) -> dict:
    out = {}
    for repo, g in df.groupby(df[C.instance_id].map(_repo)):
        per = []
        for _, h in g.groupby(C.instance_id):
            if h[C.ppr].nunique() > 1 and h[C.pagerank].nunique() > 1:
                per.append(spearmanr(h[C.ppr], h[C.pagerank]).statistic)
        out[repo] = {
            "pooled_pearson": float(pearsonr(g[C.ppr], g[C.pagerank]).statistic),
            "pooled_spearman": float(spearmanr(g[C.ppr], g[C.pagerank]).statistic),
            "per_instance_spearman_median": float(np.median(per)),
            "n_instances": len(per),
        }
    return out


def seed_top_placement(df: pd.DataFrame, seed: pd.DataFrame, weight_col: str,
                       topk: int, power: float) -> dict:
    """sharpen 후 teleport 질량이 가장 큰 파일이 PPR에서 몇 위인가."""
    seeds = dict(tuple(seed.rename(columns={weight_col: "weight"})
                       .groupby(C.instance_id)))
    ranks, set_front = [], []
    first_by_seed_rank: dict[str, int] = {}
    for iid, h in df.groupby(C.instance_id):
        s = seeds.get(iid)
        if s is None:
            continue
        s = sharpen_seed(s, topk, power)
        s = s[s[C.file_id].isin(set(h[C.file_id]))]
        if s.empty:
            continue
        top = s.sort_values("weight", ascending=False)[C.file_id].iloc[0]
        r = _rank_desc(h.set_index(C.file_id)[C.ppr])
        ranks.append(float(r[top]))
        # topk>1이면 질량이 seed 파일 여럿에 나뉜다. "seed 파일들이 PPR 맨 앞 |seed|칸을
        # 그대로 차지하는가"를 따로 센다 (비관적 순위라 동점이면 거짓 쪽).
        set_front.append(bool((r[list(s[C.file_id])] <= len(s)).all()))
        # PPR 1위 파일이 seed 몇 순위였나 (seed가 아니면 "non_seed").
        # 동점이면 file_id 사전순 첫 번째 — 결정적으로 하나만 고른다.
        first = h.sort_values([C.ppr, C.file_id], ascending=[False, True])[C.file_id].iloc[0]
        order = list(s.sort_values("weight", ascending=False)[C.file_id])
        key = f"seed_rank{order.index(first) + 1}" if first in order else "non_seed"
        first_by_seed_rank[key] = first_by_seed_rank.get(key, 0) + 1
    ranks = np.array(ranks)
    return {"n_instances": int(len(ranks)),
            "seed_top_is_ppr_rank1": float(np.mean(ranks <= 1)),
            "seed_top_in_ppr_top5": float(np.mean(ranks <= 5)),
            "seed_set_fills_ppr_front": float(np.mean(set_front)),
            "ppr_rank1_is": dict(sorted(first_by_seed_rank.items()))}


def top1_agreement(a: pd.DataFrame, a_col: str, b: pd.DataFrame, b_col: str) -> dict:
    """두 seed의 top-1 파일이 같은 인스턴스 비율 (동점이면 file_id 사전순 첫 번째)."""
    def top1(df, col):
        return (df.sort_values([C.instance_id, col, C.file_id], ascending=[True, False, True])
                  .groupby(C.instance_id)[C.file_id].first())
    ta, tb = top1(a, a_col), top1(b, b_col)
    common = ta.index.intersection(tb.index)
    return {"n_instances": int(len(common)),
            "top1_same": float((ta[common] == tb[common]).mean())}


def two_ppr_correlation(e: pd.DataFrame, b: pd.DataFrame) -> dict:
    m = e[[C.instance_id, C.file_id, C.ppr]].merge(
        b[[C.instance_id, C.file_id, C.ppr]], on=[C.instance_id, C.file_id],
        suffixes=("_embed", "_bm25"))
    per = [spearmanr(h["ppr_embed"], h["ppr_bm25"]).statistic
           for _, h in m.groupby(C.instance_id)
           if h["ppr_embed"].nunique() > 1 and h["ppr_bm25"].nunique() > 1]
    return {"n_instances": len(per), "per_instance_spearman_median": float(np.median(per)),
            "n_rows_joined": int(len(m))}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--embed-ppr", required=True, help="embed seed PPR 매트릭스")
    ap.add_argument("--bm25-ppr", required=True, help="BM25 seed PPR 매트릭스")
    ap.add_argument("--embed-seed", required=True)
    ap.add_argument("--bm25-seed", required=True)
    ap.add_argument("--seed-topk", type=int, default=1)
    ap.add_argument("--seed-power", type=float, default=1.0)
    ap.add_argument("--bm25-seed-topk", type=int, default=None,
                    help="BM25 PPR에 쓴 topk (기본 --seed-topk)")
    ap.add_argument("--bm25-seed-power", type=float, default=None)
    ap.add_argument("--out", default=None)
    a = ap.parse_args(argv)

    e = pd.read_csv(a.embed_ppr)
    b = pd.read_csv(a.bm25_ppr)
    es = pd.read_csv(a.embed_seed)
    bs = pd.read_csv(a.bm25_seed)
    btk = a.seed_topk if a.bm25_seed_topk is None else a.bm25_seed_topk
    bpw = a.seed_power if a.bm25_seed_power is None else a.bm25_seed_power

    rep = {
        "inputs": vars(a),
        "embed_ppr_vs_pagerank": ppr_vs_pagerank(e),
        "bm25_ppr_vs_pagerank": ppr_vs_pagerank(b),
        "embed_seed_top_placement": seed_top_placement(e, es, C.embed, a.seed_topk, a.seed_power),
        "bm25_seed_top_placement": seed_top_placement(b, bs, C.bm25, btk, bpw),
        "bm25_vs_embed_top1": top1_agreement(bs, C.bm25, es, C.embed),
        "embed_ppr_vs_bm25_ppr": two_ppr_correlation(e, b),
    }
    print(json.dumps(rep, indent=2, ensure_ascii=False))
    if a.out:
        with open(a.out, "w") as fh:
            json.dump(rep, fh, indent=2, ensure_ascii=False)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
