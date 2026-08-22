"""BM25 seed 평가 — 지표는 `bench.metrics.ranking`에서 **import만** 한다.

CONTRACT.md §2: 랭킹 지표의 유일한 구현은 `bench/metrics/ranking.py`(담당
`bench-harness`)다. 여기서 자체 구현하면 숫자가 갈린다.

인스턴스당 계산은 `evaluate_instance`에 위임한다 — 직접 tiebreak을 적용하면
언젠가 한 곳에서 빠뜨리고, 그러면 점수가 조용히 부풀려진다.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pandas as pd

from bench.metrics.ranking import evaluate_instance, recall_at_k
from bench.schema import C, DATA_DIR
from bench.seed import textproc
from bench.seed.dataset import load_instances

DEFAULT_KS = (1, 3, 5, 10, 20, 50)


@dataclass
class EvalResult:
    condition: str
    per_instance: pd.DataFrame
    summary: dict

    @property
    def n_instances(self) -> int:
        return len(self.per_instance)


def metrics_available() -> bool:
    try:
        recall_at_k(["a", "b"], {"a"}, 1)
    except NotImplementedError:
        return False
    return True


def evaluate_condition(seed_df: pd.DataFrame,
                       gold: dict[str, set[str]],
                       *, condition: str,
                       ks: tuple[int, ...] = DEFAULT_KS) -> EvalResult:
    """조건 하나에 대해 인스턴스별 지표를 낸다.

    Args:
        seed_df: `instance_id, file_id, bm25` 컬럼 (한 조건만).
        gold: instance_id -> gold file_id 집합. **스캔에 없는 gold도 포함**해야
            한다 — Recall 분모이자 ceiling 계산의 근거이기 때문이다.
    """
    if not metrics_available():
        raise NotImplementedError(
            "bench/metrics/ranking.py 가 아직 스텁이다. 여기서 자체 구현하지 않는다 "
            "(CONTRACT.md §2)."
        )

    rows = []
    for iid, g in seed_df.groupby(C.instance_id):
        rec = evaluate_instance(g[C.file_id].tolist(), g[C.bm25].tolist(),
                                gold.get(iid, set()), ks=ks)
        rec[C.instance_id] = iid
        rows.append(rec)

    per = pd.DataFrame(rows)
    num = per.select_dtypes("number")
    # gold가 빈 인스턴스는 NaN을 내므로 nanmean으로 집계하고 NaN 개수를 함께 남긴다.
    summary = {c: float(np.nanmean(num[c])) for c in num.columns}
    summary["_nan_counts"] = {c: int(num[c].isna().sum())
                              for c in num.columns if num[c].isna().any()}
    return EvalResult(condition=condition, per_instance=per, summary=summary)


def evaluate_all(long_df: pd.DataFrame, gold: dict[str, set[str]],
                 *, ks: tuple[int, ...] = DEFAULT_KS) -> dict[str, EvalResult]:
    return {cond: evaluate_condition(long_df[long_df["condition"] == cond],
                                     gold, condition=cond, ks=ks)
            for cond in long_df["condition"].unique()}


def format_table(results: dict[str, EvalResult],
                 ks: tuple[int, ...] = DEFAULT_KS) -> str:
    cols = ["reachable_recall_ceiling", "mrr"] + \
           [f"recall@{k}" for k in ks] + [f"ndcg@{k}" for k in ks]
    lines = ["{:<10} {:>4}".format("condition", "n")
             + "".join(f"{c.replace('reachable_recall_ceiling', 'ceiling'):>10}" for c in cols)]
    for cond in textproc.CONDITIONS:
        r = results.get(cond)
        if r is None:
            continue
        lines.append("{:<10} {:>4}".format(cond, r.n_instances)
                     + "".join(f"{r.summary[c]:>10.4f}" for c in cols))
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description="BM25 seed 평가")
    ap.add_argument("--data-dir", type=Path, default=DATA_DIR)
    ap.add_argument("--repo", default="pytest-dev/pytest")
    a = ap.parse_args(argv)

    long_df = pd.read_csv(a.data_dir / "bm25_seed_long.csv")
    gold = {i.instance_id: set(i.gold_files)
            for i in load_instances(repo=a.repo)}
    res = evaluate_all(long_df, gold)

    print("BM25 seed — bench.metrics.ranking 기준 공식 수치")
    print(f"repo={a.repo}  k1/b/k3 = 기본값 (train CV 미실시 — CONTRACT.md §3-4)")
    print()
    print(format_table(res))
    for cond, r in res.items():
        if r.summary.get("_nan_counts"):
            print(f"  NaN({cond}): {r.summary['_nan_counts']}")
    out = pd.concat([r.per_instance.assign(condition=c) for c, r in res.items()])
    out.to_csv(a.data_dir / "bm25_eval_per_instance.csv", index=False)
    print(f"\nper-instance -> {a.data_dir / 'bm25_eval_per_instance.csv'}")


if __name__ == "__main__":
    main()
