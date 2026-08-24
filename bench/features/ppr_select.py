"""스윕 결과에서 seed 샤프닝 config를 고른다. **train fold만 본다.**

무결성 규칙 §4: 하이퍼파라미터는 train 내부 CV로만 고른다.
입력 `ppr_sweep.csv`에는 애초에 train 인스턴스밖에 없고(`ppr_sweep.py` 참조),
여기서 다시 `cv_fold` 0~4로 5-fold 평균을 낸다. `cv_fold == -1`(test)은
입력에도 없고 이 코드도 참조하지 않는다.

선택 기준은 **PPR 단독 랭킹의 MRR**이다. fold별 평균을 낸 뒤 그 평균이
가장 높은 config를 고른다. fold 간 표준편차도 함께 보고한다 — fold마다
승자가 뒤집히면 그 선택은 신뢰할 수 없다.

    bench/.venv/bin/python -m bench.features.ppr_select \
        --sweep bench/data/ppr_sweep.csv \
        --splits bench/data/splits_all.csv \
        --manifest bench/data/instances_all.csv
"""

import argparse

import numpy as np
import pandas as pd

from bench.metrics.baseline import gold_sets_from_manifest
from bench.metrics.ranking import evaluate_instance
from bench.schema import C, DATA_DIR


def evaluate_config(sweep: pd.DataFrame, gold_sets: dict, ks=(1, 5, 10)) -> pd.DataFrame:
    """config × 인스턴스별 지표."""
    rows = []
    for (cfg, iid), g in sweep.groupby(["config", C.instance_id], sort=True):
        rec = evaluate_instance(g[C.file_id].tolist(), g[C.ppr].tolist(),
                                gold_sets.get(iid, set()),
                                scanned_file_ids=set(g[C.file_id]), ks=ks)
        rec["config"] = cfg
        rec[C.instance_id] = iid
        rows.append(rec)
    return pd.DataFrame(rows)


def select(per_instance: pd.DataFrame, splits: pd.DataFrame,
           metric: str = "mrr") -> pd.DataFrame:
    """fold별 평균의 평균으로 config 순위를 낸다."""
    fold = splits.set_index(C.instance_id)["cv_fold"]
    df = per_instance.copy()
    df["cv_fold"] = df[C.instance_id].map(fold)

    # test(-1)가 섞여 들어오면 선택이 오염된다. 방어적으로 잘라낸다.
    leaked = df[df["cv_fold"] < 0]
    if len(leaked):
        raise SystemExit(
            f"test 인스턴스 {leaked[C.instance_id].nunique()}개가 스윕에 섞여 있다 — "
            "§4 위반이므로 선택을 중단한다")

    per_fold = df.groupby(["config", "cv_fold"])[metric].apply(np.nanmean).reset_index()
    agg = per_fold.groupby("config")[metric].agg(["mean", "std", "count"])
    agg = agg.rename(columns={"mean": f"{metric}_cv_mean",
                              "std": f"{metric}_cv_std", "count": "n_folds"})
    return agg.sort_values(f"{metric}_cv_mean", ascending=False)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sweep", default=str(DATA_DIR / "ppr_sweep.csv"))
    ap.add_argument("--splits", default=str(DATA_DIR / "splits_all.csv"))
    ap.add_argument("--manifest", default=str(DATA_DIR / "instances_all.csv"))
    ap.add_argument("--metric", default="mrr")
    args = ap.parse_args(argv)

    sweep = pd.read_csv(args.sweep)
    splits = pd.read_csv(args.splits)
    gold = gold_sets_from_manifest(pd.read_csv(args.manifest))

    train_ids = set(splits.loc[splits["split"] == "train", C.instance_id])
    assert set(sweep[C.instance_id]) <= train_ids, "스윕에 train 밖 인스턴스가 있다"
    print(f"스윕: {sweep[C.instance_id].nunique()}개 인스턴스 "
          f"(전부 train), config {sweep['config'].nunique()}개")

    per_inst = evaluate_config(sweep, gold)
    table = select(per_inst, splits, args.metric)
    print(f"\n── train 5-fold CV ({args.metric}) ─────────────────────")
    print(table.to_string(float_format=lambda v: f"{v:.4f}"))

    best = table.index[0]
    print(f"\n선택: {best}")
    print(f"  fold 평균 {table.iloc[0][f'{args.metric}_cv_mean']:.4f} "
          f"± {table.iloc[0][f'{args.metric}_cv_std']:.4f}")

    # fold마다 승자가 뒤집히는지 — 뒤집히면 이 선택은 우연에 가깝다
    fold = splits.set_index(C.instance_id)["cv_fold"]
    d = per_inst.copy()
    d["cv_fold"] = d[C.instance_id].map(fold)
    winners = (d.groupby(["cv_fold", "config"])[args.metric].apply(np.nanmean)
                .reset_index().sort_values(args.metric, ascending=False)
                .groupby("cv_fold").first()["config"])
    print(f"  fold별 1위: {dict(winners)}")
    print(f"  → {best}가 {int((winners == best).sum())}/{len(winners)} fold에서 1위")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
