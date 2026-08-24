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
                       *, scanned: dict[str, set[str]],
                       condition: str,
                       ks: tuple[int, ...] = DEFAULT_KS) -> EvalResult:
    """조건 하나에 대해 인스턴스별 지표를 낸다.

    Args:
        seed_df: `instance_id, file_id, bm25` 컬럼 (한 조건만). 여기 담긴 것은
            **순위를 매길 후보**이며, top-N으로 잘려 있어도 된다.
        gold: instance_id -> gold file_id 집합. **스캔에 없는 gold도 포함**해야
            한다 — Recall 분모이자 ceiling 계산의 근거다.
            피처 매트릭스의 `is_positive == 1`로 복원하면 안 된다. 스캔이 못 잡은
            gold는 행 자체가 없어서 ceiling이 항상 1.0이 된다.
        scanned: instance_id -> **스캐너가 실제로 본 파일 전체**. ceiling 전용이며
            키워드 전용 필수 인자다. `seed_df`의 file_id로 대신하면, 후보를 자른
            순간 ceiling이 "스캔 상한"이 아니라 "후보집합 상한"이 되어 조용히
            틀린 값을 낸다. 두 집합은 다른 것이므로 분리해서 강제로 받는다.
    """
    if not metrics_available():
        raise NotImplementedError(
            "bench/metrics/ranking.py 가 아직 스텁이다. 여기서 자체 구현하지 않는다 "
            "(CONTRACT.md §2)."
        )

    missing = sorted(set(seed_df[C.instance_id].unique()) - set(scanned))
    if missing:
        raise ValueError(
            f"scanned 집합이 없는 인스턴스 {len(missing)}개: {missing[:5]}. "
            "ceiling을 후보집합으로 대체하지 않는다 — 스캔 목록을 넘겨라."
        )

    rows = []
    for iid, g in seed_df.groupby(C.instance_id):
        rec = evaluate_instance(g[C.file_id].tolist(), g[C.bm25].tolist(),
                                gold.get(iid, set()),
                                scanned_file_ids=scanned[iid], ks=ks)
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
                 *, scanned: dict[str, set[str]],
                 ks: tuple[int, ...] = DEFAULT_KS) -> dict[str, EvalResult]:
    return {cond: evaluate_condition(long_df[long_df["condition"] == cond],
                                     gold, scanned=scanned, condition=cond, ks=ks)
            for cond in long_df["condition"].unique()}


def scanned_sets(features: pd.DataFrame) -> dict[str, set[str]]:
    """피처 매트릭스 → {instance_id: 스캔된 file_id 전체}.

    매트릭스의 행 집합이 곧 스캐너가 본 파일이다. manifest의 `gold_files`처럼
    스캔 밖 파일을 섞으면 ceiling이 부풀지 않고 오히려 낮아진다 — 어느 쪽이든 틀린다.
    """
    return {k: set(v) for k, v in
            features.groupby(C.instance_id)[C.file_id].apply(set).items()}


def format_table(results: dict[str, EvalResult],
                 ks: tuple[int, ...] = DEFAULT_KS) -> str:
    cols = ["n_scanned", "n_candidates", "reachable_recall_ceiling", "mrr"] + \
           [f"recall@{k}" for k in ks] + [f"ndcg@{k}" for k in ks]
    def hdr(c):
        return {"reachable_recall_ceiling": "ceiling",
                "n_scanned": "scanned", "n_candidates": "cand"}.get(c, c)

    lines = ["{:<10} {:>4}".format("condition", "n")
             + "".join(f"{hdr(c):>10}" for c in cols)]
    for cond in textproc.CONDITIONS:
        r = results.get(cond)
        if r is None:
            continue
        lines.append("{:<10} {:>4}".format(cond, r.n_instances)
                     + "".join(f"{r.summary[c]:>10.1f}" if c.startswith("n_")
                               else f"{r.summary[c]:>10.4f}" for c in cols))
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description="BM25 seed 평가")
    ap.add_argument("--data-dir", type=Path, default=DATA_DIR)
    ap.add_argument("--repo", default="pydata/xarray")
    ap.add_argument("--features-csv", type=Path, default=None,
                    help="스캔된 파일 전체 집합의 출처. 기본 <data-dir>/features.csv")
    a = ap.parse_args(argv)

    long_df = pd.read_csv(a.data_dir / "bm25_seed_long.csv")
    gold = {i.instance_id: set(i.gold_files)
            for i in load_instances(repo=a.repo)}
    features = pd.read_csv(a.features_csv or (a.data_dir / "features.csv"))
    res = evaluate_all(long_df, gold, scanned=scanned_sets(features))

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
