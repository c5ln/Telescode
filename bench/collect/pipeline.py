"""인스턴스 수집 파이프라인. CONTRACT.md §4를 그대로 구현한다.

    git archive <base_commit> | tar -x         (checkout 대신 archive:
      → bench/bin/TelescodeScanner              잔여 파일 없는 깨끗한 트리가
      → bench/bin/TelescodeAlgo                 나오고 clone과 격리된다)
      → bench/features/extract.py
      → rm -f DB, rm -rf 워크트리

무결성 규칙 준수:
  §3 피처는 base_commit 트리에서만 나온다. gold patch는 라벨에만 쓰고
     트리에 적용하지 않는다.
  §5 `bench/bin/`의 pin된 바이너리만 부른다 (`bench.schema.SCANNER_BIN`).
  §6 DB는 피처 추출 직후 삭제. 동시에 존재하는 DB는 항상 1개다.
  §7 실패는 조용히 건너뛰지 않는다. 사유별로 기록해서 manifest에 남긴다.
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import pandas as pd

from bench.collect.swebench import gold_files_from_patch, load_instances
from bench.metrics.ranking import reachable_recall_ceiling
from bench.features.extract import extract
from bench.schema import ALGO_BIN, DATA_DIR, SCANNER_BIN, SCRATCH_DIR

FEATURES_CSV = DATA_DIR / "features.csv"
MANIFEST_CSV = DATA_DIR / "instances.csv"


class InstanceFailure(Exception):
    """한 인스턴스가 실패한 사유. 메시지가 manifest의 `error`로 들어간다."""


def _remove_db(db: Path) -> None:
    """DB와 SQLite 사이드카를 함께 지운다.

    `-wal`/`-shm`은 WAL 모드에서 남고 `.db`만 지우면 그대로 쌓인다.
    §6은 "DB를 지운다"이지 "`.db` 파일 하나를 지운다"가 아니다.
    """
    for suffix in ("", "-wal", "-shm", "-journal"):
        db.with_name(db.name + suffix).unlink(missing_ok=True)
    # PPR 중간 파일도 함께. 남기면 scratch가 다시 쌓인다.
    for extra in (".seed.csv", ".ppr.csv"):
        db.with_suffix(extra).unlink(missing_ok=True)


def _run(cmd: list, timeout: int) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def _export_tree(repo_dir: Path, commit: str, dest: Path, timeout: int) -> None:
    """base_commit 트리를 dest에 펼친다. 워킹트리를 건드리지 않는다."""
    dest.mkdir(parents=True, exist_ok=True)
    archive = subprocess.Popen(
        ["git", "-C", str(repo_dir), "archive", commit], stdout=subprocess.PIPE)
    untar = subprocess.Popen(["tar", "-x", "-C", str(dest)], stdin=archive.stdout)
    archive.stdout.close()
    untar.communicate(timeout=timeout)
    archive.wait(timeout=timeout)
    if archive.returncode or untar.returncode:
        raise InstanceFailure(
            f"git archive/tar 실패 (archive={archive.returncode}, tar={untar.returncode})")


def sharpen_seed(seeds: pd.DataFrame, topk: int = 0,
                 power: float = 1.0) -> pd.DataFrame:
    """teleport 분포를 뾰족하게 만든다. 기본값은 **항등 변환**이다.

    PPR의 변별력은 teleport 분포가 **집중돼 있을 때** 나온다. 실측(xarray,
    `bm25_seed__full.csv`): 후보 152개 전부가 양수 BM25를 받고, 유효 지지 크기
    (exp(entropy))가 113.2다. top-10이 전체 질량의 19%뿐이다.
    d=0.85에서 teleport 항의 기여는 15%인데 그게 거의 균등하면
    **PPR이 plain PageRank로 수렴한다.** 개인화가 씻겨 나간다.

    변환별 유효 지지 크기 (중앙값, 원래 152):
        없음 113.2 | power=2 70.9 | power=3 42.1 | top-20 19.5 | top-10 9.9

    ⚠ `topk`와 `power`는 **하이퍼파라미터다.** 무결성 규칙 §4에 따라
    train 내부 CV로만 고른다. test를 보고 고른 값은 폐기 대상이다.

    Args:
        topk: 상위 k개만 남기고 나머지는 버린다. 0이면 전부 유지.
        power: 가중치를 이 지수로 거듭제곱한다. 1.0이면 항등.
    """
    if power <= 0:
        raise ValueError(f"power는 양수여야 한다: {power}")
    out = seeds.copy()
    out["weight"] = out["weight"].astype(float).clip(lower=0.0)
    if topk and topk > 0:
        out = out.nlargest(topk, "weight")
    if power != 1.0:
        out["weight"] = out["weight"] ** power
    return out[out["weight"] > 0]


def _run_ppr(ppr_bin: Path, db: Path, seeds: pd.DataFrame,
             instance_id: str, timeout: int) -> dict[str, float]:
    """인스턴스 DB가 살아 있는 동안 PPR CLI를 돌린다.

    §6 때문에 DB는 추출 직후 사라지므로, PPR은 **반드시 여기서** 돌아야 한다.
    나중에 일괄로 못 돌린다.

    계약 (algo-core와 합의할 인터페이스):
        TelescodePPR <db_path> <seed_csv> <out_csv>
        seed_csv: 헤더 `file_id,weight` — instance_id 없음.
                  DB가 이미 인스턴스 하나에 대응하므로 불필요하다.
        out_csv:  헤더 `file_id,ppr`
                  seed에서 도달 못 한 파일은 빠져도 된다 (호출부가 0으로 채운다).
    """
    seed_csv = db.with_suffix(".seed.csv")
    out_csv = db.with_suffix(".ppr.csv")
    seeds[["file_id", "weight"]].to_csv(seed_csv, index=False)

    proc = _run([str(ppr_bin), str(db), str(seed_csv), str(out_csv)], timeout)
    if proc.returncode != 0:
        raise InstanceFailure(f"ppr rc={proc.returncode}: {proc.stderr.strip()[:200]}")
    if not out_csv.exists():
        raise InstanceFailure("PPR CLI가 출력 CSV를 만들지 않았다")

    out = pd.read_csv(out_csv)
    missing = {"file_id", "ppr"} - set(out.columns)
    if missing:
        raise InstanceFailure(f"PPR 출력에 컬럼 없음: {sorted(missing)}")
    return dict(zip(out["file_id"], out["ppr"].astype(float)))


def process_instance(row, repo_dir: Path, work_root: Path,
                     timeout: int = 900,
                     ppr_bin: Path | None = None,
                     seeds_by_instance: dict | None = None,
                     seed_topk: int = 0,
                     seed_power: float = 1.0) -> tuple[pd.DataFrame, dict]:
    """한 인스턴스를 처리하고 (피처 행, manifest 행)을 돌려준다.

    실패하면 `InstanceFailure`를 던진다. 호출자가 사유를 기록한다.
    """
    instance_id = row.instance_id
    gold = gold_files_from_patch(row.patch)
    if not gold.all:
        raise InstanceFailure("gold patch에서 파일 경로를 못 뽑았다")

    tree = work_root / instance_id
    db = SCRATCH_DIR / f"{instance_id}.db"
    shutil.rmtree(tree, ignore_errors=True)
    _remove_db(db)

    t0 = time.monotonic()
    try:
        _export_tree(repo_dir, row.base_commit, tree, timeout)

        scan = _run([str(SCANNER_BIN), str(tree), str(db)], timeout)
        if scan.returncode != 0:
            raise InstanceFailure(f"scanner rc={scan.returncode}: {scan.stderr.strip()[:200]}")
        if not db.exists():
            raise InstanceFailure("scanner가 DB를 만들지 않았다")

        algo = _run([str(ALGO_BIN), str(db)], timeout)
        if algo.returncode != 0:
            raise InstanceFailure(f"algo rc={algo.returncode}: {algo.stderr.strip()[:200]}")

        ppr_values = None
        if ppr_bin is not None:
            seeds = (seeds_by_instance or {}).get(instance_id)
            if seeds is None or seeds.empty:
                raise InstanceFailure("PPR을 요청했는데 이 인스턴스의 seed가 없다")
            seeds = sharpen_seed(seeds, seed_topk, seed_power)
            if seeds.empty:
                raise InstanceFailure(
                    "seed 샤프닝 후 양수 가중치가 없다 (PPR CLI가 rc=5를 낼 상황)")
            ppr_values = _run_ppr(ppr_bin, db, seeds, instance_id, timeout)

        # commit_skew: 인스턴스마다 자기 base_commit을 직접 스캔하므로 0이다.
        df = extract(db, instance_id, gold.all, commit_skew=0,
                     ppr_values=ppr_values)
        if df.empty:
            raise InstanceFailure("스캔된 파일이 0개다")
        if df["combined"].isna().any():
            # reading_sequence LEFT JOIN이 비었다 = algo가 그 파일을 안 봤다.
            n = int(df["combined"].isna().sum())
            raise InstanceFailure(f"reading_sequence 누락 {n}행 — algo 미완료 의심")
    finally:
        # §6: DB와 트리는 성공/실패 무관하게 즉시 지운다.
        _remove_db(db)
        shutil.rmtree(tree, ignore_errors=True)

    scanned = set(df["file_id"])
    manifest = {
        "instance_id": instance_id,
        "base_commit": row.base_commit,
        "status": "ok",
        "error": "",
        # gold **전체**. 스캔에 없는 gold는 피처 매트릭스에 행이 없으므로,
        # 이걸 안 남기면 downstream이 Recall 분모를 gold∩scan으로 잡게 되고
        # ceiling이 항상 1.0인 무의미한 값이 된다. 실제로 그렇게 틀렸었다.
        "gold_files": ";".join(sorted(gold.all)),
        "n_files_scanned": len(df),
        "n_gold": len(gold.all),
        "n_gold_in_scan": len(gold.all & scanned),
        "n_gold_added_by_patch": len(gold.added),   # base_commit에 없던 신규 파일
        "n_gold_existing_at_base": len(gold.existing_at_base),
        "reachable_recall_ceiling": reachable_recall_ceiling(scanned, gold.all),
        "n_positive_rows": int(df["is_positive"].sum()),
        "seconds": round(time.monotonic() - t0, 2),   # 벽시계는 NTP 보정에 튄다
    }

    # 조인 검증 (schema.normalize_file_id docstring). 라벨 행 수와 집합 크기가
    # 어긋나면 정규화가 한쪽만 걸린 것이다.
    if manifest["n_positive_rows"] != manifest["n_gold_in_scan"]:
        raise InstanceFailure(
            f"라벨 조인 불일치: positive행={manifest['n_positive_rows']} "
            f"gold∩scan={manifest['n_gold_in_scan']}")

    return df, manifest


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", default="pytest-dev/pytest")
    ap.add_argument("--repo-dir", required=True, help="해당 repo의 로컬 clone")
    ap.add_argument("--split", default="test")
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--work-root", default=None,
                    help="트리를 펼칠 임시 디렉토리 (기본: 시스템 임시)")
    ap.add_argument("--out", default=str(FEATURES_CSV))
    ap.add_argument("--manifest", default=str(MANIFEST_CSV))
    ap.add_argument("--ppr-bin", default=None,
                    help="Phase 3 PPR CLI. 주면 인스턴스 DB가 살아 있는 동안 실행한다")
    ap.add_argument("--seed-csv", default=None,
                    help="long CSV: instance_id, file_id, <weight-col>. --ppr-bin과 함께 쓴다")
    ap.add_argument("--seed-weight-col", default="bm25",
                    help="seed CSV에서 teleport 가중치로 쓸 컬럼 (기본 bm25)")
    ap.add_argument("--seed-topk", type=int, default=0,
                    help="teleport를 상위 k개로 절단. 0=전부. **§4 하이퍼파라미터**")
    ap.add_argument("--seed-power", type=float, default=1.0,
                    help="teleport 가중치 거듭제곱. 1.0=항등. **§4 하이퍼파라미터**")
    args = ap.parse_args(argv)

    for path in (SCANNER_BIN, ALGO_BIN):
        if not path.exists():
            print(f"pin된 바이너리 없음: {path}", file=sys.stderr)
            return 2

    repo_dir = Path(args.repo_dir).resolve()
    instances = load_instances(args.repo, args.split)
    if args.limit:
        instances = instances.head(args.limit)
    print(f"{args.repo}: {len(instances)}개 인스턴스", flush=True)

    SCRATCH_DIR.mkdir(parents=True, exist_ok=True)
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    work_root = Path(args.work_root) if args.work_root else Path(
        tempfile.mkdtemp(prefix="bench-trees-"))
    work_root.mkdir(parents=True, exist_ok=True)

    ppr_bin = Path(args.ppr_bin).resolve() if args.ppr_bin else None
    seeds_by_instance = None
    if ppr_bin is not None:
        if not ppr_bin.exists():
            print(f"PPR 바이너리 없음: {ppr_bin}", file=sys.stderr)
            return 2
        if not args.seed_csv:
            print("--ppr-bin에는 --seed-csv가 필요하다", file=sys.stderr)
            return 2
        seed_df = pd.read_csv(args.seed_csv)
        seed_df = seed_df.rename(columns={args.seed_weight_col: "weight"})
        for need in ("instance_id", "file_id", "weight"):
            if need not in seed_df.columns:
                print(f"seed CSV에 컬럼 없음: {need}", file=sys.stderr)
                return 2
        seeds_by_instance = dict(tuple(seed_df.groupby("instance_id")))
        print(f"seed: {args.seed_csv} — {len(seeds_by_instance)}개 인스턴스")

    frames, manifests = [], []
    for n, (_, row) in enumerate(instances.iterrows(), start=1):
        try:
            df, manifest = process_instance(
                row, repo_dir, work_root,
                ppr_bin=ppr_bin, seeds_by_instance=seeds_by_instance,
                seed_topk=args.seed_topk, seed_power=args.seed_power)
            frames.append(df)
            manifests.append(manifest)
            print(f"[{n}/{len(instances)}] ok   {row.instance_id} "
                  f"files={manifest['n_files_scanned']} "
                  f"gold={manifest['n_gold']} "
                  f"ceiling={manifest['reachable_recall_ceiling']:.2f}", flush=True)
        except Exception as exc:                      # noqa: BLE001 — §7
            manifests.append({
                "instance_id": row.instance_id,
                "base_commit": row.base_commit,
                "status": "failed",
                "error": f"{type(exc).__name__}: {exc}"[:300],
            })
            print(f"[{n}/{len(instances)}] FAIL {row.instance_id}: {exc}",
                  file=sys.stderr, flush=True)

    manifest_df = pd.DataFrame(manifests)
    manifest_df.to_csv(args.manifest, index=False)

    n_ok = int((manifest_df["status"] == "ok").sum())
    n_fail = len(manifest_df) - n_ok
    if frames:
        features = pd.concat(frames, ignore_index=True)
        features.to_csv(args.out, index=False)
        print(f"\n피처 {len(features)}행 → {args.out}")
    print(f"manifest → {args.manifest}")
    print(f"성공 {n_ok} / 실패 {n_fail} / 전체 {len(manifest_df)}")   # §7
    if n_fail:
        print("\n실패 사유:", file=sys.stderr)
        for _, r in manifest_df[manifest_df["status"] == "failed"].iterrows():
            print(f"  {r.instance_id}: {r.error}", file=sys.stderr)

    leftover = [p for p in SCRATCH_DIR.iterdir() if p.name != ".gitkeep"]
    if leftover:
        print(f"경고: scratch에 DB가 남았다: {leftover}", file=sys.stderr)   # §6
    shutil.rmtree(work_root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
