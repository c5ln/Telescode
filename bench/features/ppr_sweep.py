"""seed 샤프닝 하이퍼파라미터 스윕. **train 인스턴스에서만 돈다.**

## 왜 스윕이 필요한가

BM25 seed를 그대로 teleport로 쓰면 후보 전부가 양수 가중치를 받는다
(xarray 실측: 152/152, 유효 지지 크기 113.2). d=0.85에서 teleport 기여가 15%인데
그게 거의 균등하면 **PPR이 plain PageRank로 수렴**해서 `ppr` 컬럼이 `pagerank`
컬럼과 사실상 중복된다. 즉 아무것도 측정하지 못한다.
`sharpen_seed`의 `topk`/`power`가 그걸 조절한다.

## 왜 train만 도는가

`topk`/`power`는 하이퍼파라미터다. 무결성 규칙 §4에 따라 **train 내부 CV로만**
고른다. 이 스크립트는 `--splits`의 `split == "train"` 인스턴스만 처리하므로,
test 인스턴스는 스윕 단계에서 **한 번도 스캔되지 않는다.** 선택 근거가
test를 볼 수 없다는 것을 파일 존재 자체로 증명하려는 것이다.
최종 생산 실행은 고른 값 하나로 `bench.collect.pipeline`이 229개 전부를 돈다.

## 왜 한 번의 스캔에 여러 PPR인가

그래프는 config와 무관하다. 인스턴스당 스캔+algo(약 1.5초)를 config 수만큼
반복할 이유가 없다. DB가 살아 있는 동안 config마다 PPR(약 0.13초)만 다시 돌고,
DB는 §6대로 즉시 삭제된다.

    bench/.venv/bin/python -m bench.features.ppr_sweep \
        --repo-dir <clone> --repo pydata/xarray \
        --seed bench/data/seed_all/bm25_seed__no_paths.csv \
        --splits bench/data/splits_all.csv \
        --out bench/data/ppr_sweep.csv
"""

import argparse
import shutil
import sys
import tempfile
from pathlib import Path

import pandas as pd

from bench.collect.pipeline import (
    InstanceFailure,
    _export_tree,
    _remove_db,
    _run,
    _run_ppr,
    sharpen_seed,
)
from bench.collect.swebench import load_instances
from bench.schema import ALGO_BIN, C, DATA_DIR, SCANNER_BIN, SCRATCH_DIR

# (topk, power). topk=0은 전부 유지, power=1.0은 항등 = 현재 동작.
# 항등을 반드시 포함한다 — 샤프닝이 실제로 필요한지가 측정 대상이기 때문이다.
DEFAULT_GRID = [
    (0, 1.0), (0, 2.0), (0, 3.0),
    (50, 1.0), (20, 1.0), (10, 1.0), (5, 1.0),
    (20, 2.0), (10, 2.0),
]


def config_name(topk: int, power: float) -> str:
    return f"topk{topk}_pow{power:g}"


def sweep_instance(row, repo_dir: Path, work_root: Path, ppr_bin: Path,
                   seeds: pd.DataFrame, grid, timeout: int = 900) -> list[pd.DataFrame]:
    """한 인스턴스를 한 번만 스캔하고 config마다 PPR을 돌린다."""
    iid = row.instance_id
    tree = work_root / iid
    db = SCRATCH_DIR / f"{iid}.db"
    shutil.rmtree(tree, ignore_errors=True)
    _remove_db(db)

    out = []
    try:
        _export_tree(repo_dir, row.base_commit, tree, timeout)
        scan = _run([str(SCANNER_BIN), str(tree), str(db)], timeout)
        if scan.returncode != 0:
            raise InstanceFailure(f"scanner rc={scan.returncode}")
        algo = _run([str(ALGO_BIN), str(db)], timeout)
        if algo.returncode != 0:
            raise InstanceFailure(f"algo rc={algo.returncode}")

        for topk, power in grid:
            sharp = sharpen_seed(seeds, topk, power)
            if sharp.empty:
                raise InstanceFailure(f"{config_name(topk, power)}: 샤프닝 후 seed 없음")
            values = _run_ppr(ppr_bin, db, sharp, iid, timeout)
            out.append(pd.DataFrame({
                C.instance_id: iid,
                C.file_id: list(values),
                "config": config_name(topk, power),
                C.ppr: list(values.values()),
            }))
    finally:
        _remove_db(db)                       # §6
        shutil.rmtree(tree, ignore_errors=True)
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", action="append", required=True,
                    help="repo 이름. --repo-dir와 같은 순서로 반복 지정")
    ap.add_argument("--repo-dir", action="append", required=True)
    ap.add_argument("--seed", required=True)
    ap.add_argument("--splits", required=True)
    ap.add_argument("--seed-weight-col", default="bm25")
    ap.add_argument("--out", default=str(DATA_DIR / "ppr_sweep.csv"))
    ap.add_argument("--grid", default=None,
                    help='커스텀 그리드: "topk:power,topk:power". 미지정 시 DEFAULT_GRID')
    args = ap.parse_args(argv)

    if len(args.repo) != len(args.repo_dir):
        print("--repo와 --repo-dir 개수가 다르다", file=sys.stderr)
        return 2
    ppr_bin = Path(__file__).resolve().parents[1] / "bin" / "TelescodePPR"
    if not ppr_bin.exists():
        print(f"pin된 PPR 바이너리 없음: {ppr_bin}", file=sys.stderr)
        return 2

    grid = DEFAULT_GRID
    if args.grid:
        grid = [(int(t), float(w)) for t, w in
                (pair.split(":") for pair in args.grid.split(","))]
    print(f"config {len(grid)}개: {[config_name(t, w) for t, w in grid]}")

    splits = pd.read_csv(args.splits)
    train_ids = set(splits.loc[splits["split"] == "train", C.instance_id])
    print(f"train 인스턴스 {len(train_ids)}개만 처리한다 (test는 스캔조차 하지 않는다)")

    seed_df = pd.read_csv(args.seed).rename(columns={args.seed_weight_col: "weight"})
    seeds_by = dict(tuple(seed_df.groupby(C.instance_id)))

    SCRATCH_DIR.mkdir(parents=True, exist_ok=True)
    work_root = Path(tempfile.mkdtemp(prefix="ppr-sweep-"))
    frames, n_ok, failures = [], 0, []

    for repo, repo_dir in zip(args.repo, args.repo_dir):
        inst = load_instances(repo)
        inst = inst[inst.instance_id.isin(train_ids)]
        print(f"{repo}: {len(inst)}개")
        for n, (_, row) in enumerate(inst.iterrows(), start=1):
            seeds = seeds_by.get(row.instance_id)
            if seeds is None or seeds.empty:
                failures.append((row.instance_id, "seed 없음"))
                print(f"  [{n}/{len(inst)}] FAIL {row.instance_id}: seed 없음", file=sys.stderr)
                continue
            try:
                frames.extend(sweep_instance(row, Path(repo_dir).resolve(), work_root,
                                             ppr_bin, seeds, grid))
                n_ok += 1
                if n % 20 == 0:
                    print(f"  [{n}/{len(inst)}] ok", flush=True)
            except Exception as exc:                      # noqa: BLE001 — §7
                failures.append((row.instance_id, str(exc)))
                print(f"  [{n}/{len(inst)}] FAIL {row.instance_id}: {exc}",
                      file=sys.stderr, flush=True)

    shutil.rmtree(work_root, ignore_errors=True)
    if frames:
        pd.concat(frames, ignore_index=True).to_csv(args.out, index=False)
        print(f"\n→ {args.out}")
    print(f"성공 {n_ok} / 실패 {len(failures)}")           # §7
    for iid, why in failures:
        print(f"  {iid}: {why}", file=sys.stderr)
    leftover = [p for p in SCRATCH_DIR.iterdir() if p.name != ".gitkeep"]
    if leftover:
        print(f"경고: scratch 잔여 {leftover}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
