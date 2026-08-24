"""BM25 seed → 피처 매트릭스 조인. **커밋된 유일한 경로다.**

이전에는 이 조인이 대화형으로 한 번 돌린 산출물로만 존재했다
(`features_with_bm25.csv`를 만드는 코드가 저장소에 없었다). 그 매트릭스는
재현이 불가능하고, 재스캔하면 조용히 낡은 값이 된다. Phase 4가 그런 매트릭스로
학습하는 것을 막는 게 이 파일의 목적이다.

조인 검증은 `bench.seed.join_check`를 **재구현하지 않고 호출한다** — 구현이
두 개면 어느 쪽이 맞는지 알 수 없게 된다 (CONTRACT.md §2와 같은 이유).
`JoinReport.ok()`가 거짓이면 **출력을 쓰지 않고 실패한다.** 경고만 찍고
넘어가면 `bm25`가 통째로 비어 있는 매트릭스가 그대로 하류로 흘러간다.
실제로 재스캔 직후 두 매트릭스 다 `bm25` non-null이 0이었다.

    bench/.venv/bin/python -m bench.features.join_bm25 \
        --features bench/data/features.csv \
        --seed     bench/data/bm25_seed__full.csv \
        --out      bench/data/features_with_bm25.csv
"""

import argparse
import sys

import pandas as pd

from bench.schema import ALL_COLUMNS, C, DATA_DIR
from bench.seed.join_check import join_seed


def join(features: pd.DataFrame, seed: pd.DataFrame,
         min_coverage: float = 0.80) -> tuple[pd.DataFrame, object]:
    """bm25/bm25_rank를 붙이고 `ALL_COLUMNS` 순서로 되돌린다.

    컬럼 순서를 강제하는 이유: 순서가 어긋난 매트릭스는 "손으로 만든 것"이라는
    신호였다. 스키마 순서로 고정해두면 그 구분이 눈에 보인다.
    """
    merged, report = join_seed(features, seed)
    if not report.ok(min_coverage=min_coverage):
        raise SystemExit(
            f"조인 검증 실패 — 출력을 쓰지 않는다.\n"
            f"  매칭 {report.n_matched} / 피처 {report.n_features} / seed {report.n_seed}\n"
            f"  커버리지 {report.coverage:.4f} (임계 {min_coverage})\n"
            f"  bm25가 하나도 없는 인스턴스 {len(report.instances_zero_match)}개: "
            f"{report.instances_zero_match[:5]}\n"
            f"  seed에만 있는 키 {report.seed_only} / 매트릭스에만 있는 키 {report.features_only}\n"
            f"→ file_id 정규화가 어긋났거나, seed가 다른 스캔본에서 나왔다."
        )

    missing = [c for c in ALL_COLUMNS if c not in merged.columns]
    if missing:
        raise SystemExit(f"조인 후 스키마 컬럼 누락: {missing}")
    extra = [c for c in merged.columns if c not in ALL_COLUMNS]
    return merged[ALL_COLUMNS + extra], report


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--features", default=str(DATA_DIR / "features.csv"))
    ap.add_argument("--seed", default=str(DATA_DIR / "bm25_seed__full.csv"))
    ap.add_argument("--out", default=str(DATA_DIR / "features_with_bm25.csv"))
    ap.add_argument("--min-coverage", type=float, default=0.80)
    args = ap.parse_args(argv)

    features = pd.read_csv(args.features)
    seed = pd.read_csv(args.seed)
    merged, rep = join(features, seed, min_coverage=args.min_coverage)

    merged.to_csv(args.out, index=False)
    print(f"{args.features} ({len(features)}행) + {args.seed} ({len(seed)}행)")
    print(f"  매칭 {rep.n_matched}  커버리지 {rep.coverage:.4f}  "
          f"bm25 있는 인스턴스 {rep.instances_matched}")
    print(f"  seed에만 {rep.seed_only} / 매트릭스에만 {rep.features_only}")
    print(f"  bm25 non-null {int(merged[C.bm25].notna().sum())} / {len(merged)}")
    print(f"→ {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
