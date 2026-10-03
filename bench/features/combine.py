"""여러 repo의 피처 매트릭스를 하나로 합친다.

구간별 분석(어휘 중첩 등)에서 **구간 크기가 결론의 신뢰도를 좌우한다.**
xarray 단독으로는 non_overlap 구간이 16 인스턴스로 노이즈 기준(20) 아래였다.
pytest 119를 합치면 그 문제가 해소된다.

`instance_id`가 repo 접두사를 포함해(`pydata__xarray-*`, `pytest-dev__pytest-*`)
전역 고유하므로 단순 concat이 안전하다. 그래도 중복은 명시적으로 확인한다 —
같은 인스턴스가 두 번 들어오면 랭킹 group이 오염되고, train/test 분할이
누출된다.

`repo` 컬럼을 덧붙인다. 피처가 아니라 **구간 분석용 메타**다
(`EXCLUDED_FROM_TRAINING`에 준해 학습 입력에서 빼야 한다).

    bench/.venv/bin/python -m bench.features.combine \
        --features bench/data/features.csv bench/data/features_pytest.csv \
        --manifests bench/data/instances.csv bench/data/instances_pytest.csv \
        --out bench/data/features_all.csv \
        --manifest-out bench/data/instances_all.csv \
        --splits-out bench/data/splits_all.csv
"""

import argparse
import sys

import pandas as pd

from bench.metrics.baseline import make_splits
from bench.schema import ALL_COLUMNS, C, DATA_DIR

REPO_COLUMN = "repo"


def _repo_of(instance_id: str) -> str:
    """`pydata__xarray-7444` → `pydata/xarray`. SWE-bench ID 규약."""
    head = instance_id.rsplit("-", 1)[0]
    return head.replace("__", "/", 1)


def combine(frames: list[pd.DataFrame], names: list[str]) -> pd.DataFrame:
    out = []
    for df, name in zip(frames, names):
        missing = [c for c in ALL_COLUMNS if c not in df.columns]
        if missing:
            raise SystemExit(f"{name}: 스키마 컬럼 누락 {missing}")
        keep = ALL_COLUMNS + [c for c in df.columns if c not in ALL_COLUMNS]
        out.append(df[keep])

    merged = pd.concat(out, ignore_index=True)
    merged[REPO_COLUMN] = merged[C.instance_id].map(_repo_of)

    dup = merged.duplicated(subset=[C.instance_id, C.file_id]).sum()
    if dup:
        raise SystemExit(
            f"(instance_id, file_id) 중복 {dup}행 — 같은 인스턴스가 두 번 들어왔다. "
            "랭킹 group이 오염되고 분할이 누출된다.")
    return merged


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--features", nargs="+", required=True)
    ap.add_argument("--manifests", nargs="+", default=[])
    ap.add_argument("--out", default=str(DATA_DIR / "features_all.csv"))
    ap.add_argument("--manifest-out", default=str(DATA_DIR / "instances_all.csv"))
    ap.add_argument("--splits-out", default=str(DATA_DIR / "splits_all.csv"))
    args = ap.parse_args(argv)

    frames = [pd.read_csv(p) for p in args.features]
    merged = combine(frames, args.features)
    merged.to_csv(args.out, index=False)

    per_repo = merged.groupby(REPO_COLUMN)[C.instance_id].nunique()
    print(f"피처 {len(merged)}행 / {merged[C.instance_id].nunique()}개 인스턴스 → {args.out}")
    for repo, n in per_repo.items():
        print(f"  {repo}: {n}")

    if args.manifests:
        mans = [pd.read_csv(p) for p in args.manifests]
        man = pd.concat(mans, ignore_index=True)
        if man[C.instance_id].duplicated().any():
            print("manifest에 중복 instance_id가 있다", file=sys.stderr)
            return 2
        man.to_csv(args.manifest_out, index=False)
        ok = int((man["status"] == "ok").sum())
        print(f"manifest {len(man)}행 (성공 {ok} / 실패 {len(man) - ok}) → {args.manifest_out}")

    # 분할은 **합친 전체에 대해 다시** 만든다. repo별 분할을 이어붙이면
    # 비율이 어긋난다. 해시 기반이라 기존 배정은 그대로 유지된다.
    splits = make_splits(merged[C.instance_id])
    splits.to_csv(args.splits_out, index=False)
    n_tr = int((splits.split == "train").sum())
    print(f"분할 → {args.splits_out}: train {n_tr} / test {len(splits) - n_tr}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
