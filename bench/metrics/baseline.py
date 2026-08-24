"""단일 피처 랭킹 baseline 평가 + train/test 분할 생성.

Phase 2·4가 "좋아졌다"고 말하려면 비교 대상이 필요하다. 여기서 만드는 것은
**학습 없는 baseline**이다: 피처 하나로 파일을 정렬했을 때의 Recall/MRR/NDCG.

모든 지표는 `bench.metrics.ranking`을 통해서만 계산한다 (CONTRACT.md §2).
정렬은 전부 `apply_pessimistic_tiebreak`을 거친다 — xarray/pytest 둘 다
`pagerank`가 대다수 파일에서 균일 바닥값이라 동점 처리가 결과를 좌우한다.

    bench/.venv/bin/python -m bench.metrics.baseline --features bench/data/features.csv
"""

import argparse
import hashlib

import numpy as np
import pandas as pd

from bench.metrics.ranking import evaluate_instance
from bench.schema import C, DATA_DIR

KS = (1, 3, 5, 10, 20, 50)

# (표시 이름, 컬럼, 내림차순인가)
# file_rank만 오름차순이다 — 1이 "가장 먼저 읽어라"이므로 점수로는 -rank가 맞다.
#
# `combined`은 **baseline이 아니라 "현행 제품 설정"**으로만 읽는다.
# Phase 0이 gamma=1.5로 ease(=1-complexity) 항을 넣어 점수의 60%가 "쉬운 정도"라,
# "버그가 어디 있나"에는 구조적으로 반대 방향이다. 비교 기준으로 쓰면 이후
# PPR/BM25/LTR의 개선폭이 전부 "baseline을 반대로 돌려놔서 생긴 이득"이 된다.
# 정당한 그래프 baseline은 아래 B1/B2다.
BASELINES = [
    ("file_rank (제품 현재 출력)", C.file_rank,   False),
    ("combined (제품 현재 설정)",  C.combined,    True),
    ("pagerank",                   C.pagerank,    True),
    ("bc",                         C.bc,          True),
    ("in_deg",                     C.in_deg,      True),
    ("out_deg",                    C.out_deg,     True),
    ("complexity",                 C.complexity,  True),
    ("logical_loc",                C.logical_loc, True),
]

# B2는 단일 컬럼이 아니라 두 컬럼의 조합이라 센티넬로 표시한다.
B2_SENTINEL = "__b2__"
BASELINES.insert(2, ("B2 = 0.6·PR + 0.4·BC (gamma 없음)", B2_SENTINEL, True))


# gamma 항이 없는 재구성 baseline. 매트릭스의 원시 컬럼만으로 만들어지므로
# 재스캔 없이 언제든 복원된다 — 원시 컬럼을 반드시 보존하는 이유다.
#   B1 = pagerank 단독            (위 BASELINES에 이미 있다)
#   B2 = alpha·norm(PR) + beta·norm(BC),  alpha=0.6, beta=0.4
B2_ALPHA, B2_BETA = 0.6, 0.4


def minmax(x: np.ndarray) -> np.ndarray:
    """인스턴스 안에서의 min-max 정규화. 전 구간 동일값이면 0을 준다."""
    lo, hi = np.nanmin(x), np.nanmax(x)
    if not np.isfinite(lo) or not np.isfinite(hi) or hi <= lo:
        return np.zeros_like(x, dtype=float)
    return (x - lo) / (hi - lo)


def b2_score(group: pd.DataFrame) -> np.ndarray:
    """B2 = 0.6·norm(pagerank) + 0.4·norm(bc). gamma(ease) 항 없음.

    정규화는 **인스턴스 안에서** 한다. 랭킹은 인스턴스별로 매기므로
    전역 정규화는 인스턴스 간 스케일 차이를 끌어들일 뿐이다.
    """
    return (B2_ALPHA * minmax(group[C.pagerank].to_numpy(dtype=float))
            + B2_BETA * minmax(group[C.bc].to_numpy(dtype=float)))


def _stable_hash(s: str) -> int:
    """instance_id → 안정적인 정수. 파이썬 `hash()`는 실행마다 바뀐다."""
    return int(hashlib.sha256(s.encode()).hexdigest()[:16], 16)


def make_splits(instance_ids, n_folds: int = 5, test_frac: float = 0.25) -> pd.DataFrame:
    """**instance_id 단위** train/test 분할 (무결성 규칙 §1).

    파일 단위로 나누면 같은 인스턴스의 파일이 양쪽에 걸쳐 누수가 된다.
    해시 기반이라 인스턴스가 추가돼도 기존 배정이 안 흔들린다.

    `cv_fold`는 train 안에서만 의미가 있다. 하이퍼파라미터는 이 fold로만
    고른다 (§4). test를 보고 고른 숫자는 폐기 대상이다.
    """
    rows = []
    for iid in sorted(set(instance_ids)):
        h = _stable_hash(iid)
        is_test = (h % 1000) < int(test_frac * 1000)
        rows.append({
            C.instance_id: iid,
            "split": "test" if is_test else "train",
            "cv_fold": -1 if is_test else (h // 1000) % n_folds,
        })
    return pd.DataFrame(rows)


def gold_sets_from_manifest(manifest: pd.DataFrame) -> dict[str, set[str]]:
    """manifest의 `gold_files` → {instance_id: 전체 gold 집합}.

    **피처 매트릭스에서 gold를 복원하면 안 된다.** 스캔이 못 잡은 gold는
    행 자체가 없어서 `is_positive == 1`을 모으면 `gold ∩ scan`만 나온다.
    그러면 Recall 분모가 줄어 점수가 부풀고 ceiling은 항상 1.0이 된다.
    """
    return {
        r[C.instance_id]: set(str(r["gold_files"]).split(";")) if r["gold_files"] else set()
        for _, r in manifest[manifest["status"] == "ok"].iterrows()
    }


def evaluate_baselines(df: pd.DataFrame, gold_sets: dict[str, set[str]],
                       exclude_generated: bool = False) -> pd.DataFrame:
    """각 단일 피처 랭킹의 인스턴스 평균 지표.

    Args:
        df: 피처 매트릭스.
        gold_sets: `gold_sets_from_manifest` 결과. **전체** gold여야 한다.
            분모 규약은 `bench.metrics.ranking` 모듈 docstring 참조.
        exclude_generated: `is_generated` 파일을 **후보에서** 뺀다.
            피처로 쓰는 게 아니라 후보 필터다 (CONTRACT.md §1 메타 표).
    """
    results = []
    for name, col, descending in BASELINES:
        per_instance = []
        for iid, g in df.groupby(C.instance_id, sort=True):
            if exclude_generated:
                g = g[g[C.is_generated] == 0]
            if g.empty:
                continue
            gold = gold_sets[iid]
            if col is B2_SENTINEL:
                scores = b2_score(g)
            else:
                scores = g[col].to_numpy(dtype=float)
                if not descending:
                    scores = -scores
            scanned = set(g[C.file_id])
            per_instance.append(evaluate_instance(
                g[C.file_id].tolist(), scores, gold,
                scanned_file_ids=scanned, ks=KS))

        m = pd.DataFrame(per_instance)
        row = {"baseline": name, "n_instances": len(m),
               "n_nan_instances": int(m["mrr"].isna().sum())}
        row["ceiling"] = float(np.nanmean(m["reachable_recall_ceiling"]))
        row["MRR"] = float(np.nanmean(m["mrr"]))
        for k in KS:
            row[f"R@{k}"] = float(np.nanmean(m[f"recall@{k}"]))
        for k in (5, 10):
            row[f"NDCG@{k}"] = float(np.nanmean(m[f"ndcg@{k}"]))
        results.append(row)

    return pd.DataFrame(results)


def random_floor(df: pd.DataFrame, gold_sets: dict[str, set[str]],
                 n_trials: int = 20, seed: int = 42) -> dict:
    """무작위 정렬의 기대 성능. baseline이 이걸 못 넘으면 신호가 없는 것이다."""
    rng = np.random.default_rng(seed)
    acc = []
    for _ in range(n_trials):
        per_instance = []
        for iid, g in df.groupby(C.instance_id, sort=True):
            gold = gold_sets[iid]
            per_instance.append(evaluate_instance(
                g[C.file_id].tolist(), rng.random(len(g)), gold,
                scanned_file_ids=set(g[C.file_id]), ks=KS))
        m = pd.DataFrame(per_instance)
        acc.append({"MRR": np.nanmean(m["mrr"]),
                    **{f"R@{k}": np.nanmean(m[f"recall@{k}"]) for k in KS}})
    return pd.DataFrame(acc).mean().to_dict()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--features", default=str(DATA_DIR / "features.csv"))
    ap.add_argument("--manifest", default=str(DATA_DIR / "instances.csv"),
                    help="gold 전체 집합의 출처. 피처 CSV에서 복원하면 분모가 틀린다")
    ap.add_argument("--splits-out", default=str(DATA_DIR / "splits.csv"))
    ap.add_argument("--out", default=None)
    args = ap.parse_args(argv)

    df = pd.read_csv(args.features)
    manifest = pd.read_csv(args.manifest).fillna({"gold_files": ""})
    gold_sets = gold_sets_from_manifest(manifest)

    missing = set(df[C.instance_id]) - set(gold_sets)
    if missing:
        raise SystemExit(f"manifest에 없는 인스턴스 {len(missing)}개: {sorted(missing)[:5]}")

    n_gold_total = sum(len(v) for v in gold_sets.values())
    print(f"{args.features}: {len(df)}행, "
          f"{df[C.instance_id].nunique()}개 인스턴스, "
          f"positive {int(df[C.is_positive].sum())}행")
    print(f"{args.manifest}: gold {n_gold_total}개 "
          f"(스캔에 있는 것 {int(df[C.is_positive].sum())}개 — "
          f"차이 {n_gold_total - int(df[C.is_positive].sum())}개가 Recall 상한을 깎는다)")

    splits = make_splits(df[C.instance_id])
    splits.to_csv(args.splits_out, index=False)
    print(f"분할 → {args.splits_out}: "
          f"train {int((splits.split == 'train').sum())} / "
          f"test {int((splits.split == 'test').sum())}\n")

    for label, exclude in [("전체 후보", False), ("is_generated 제외", True)]:
        res = evaluate_baselines(df, gold_sets, exclude_generated=exclude)
        print(f"── {label} ─────────────────────────────────────────")
        print(res.to_string(index=False, float_format=lambda v: f"{v:.4f}"))
        print()
        if args.out and not exclude:
            res.to_csv(args.out, index=False)

    floor = random_floor(df, gold_sets)
    print("── 무작위 정렬 (20회 평균) ────────────────────────────")
    print("  " + "  ".join(f"{k}={v:.4f}" for k, v in floor.items()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
