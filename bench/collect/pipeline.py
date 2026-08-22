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


def process_instance(row, repo_dir: Path, work_root: Path,
                     timeout: int = 900) -> tuple[pd.DataFrame, dict]:
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

        # commit_skew: 인스턴스마다 자기 base_commit을 직접 스캔하므로 0이다.
        df = extract(db, instance_id, gold.all, commit_skew=0)
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

    frames, manifests = [], []
    for n, (_, row) in enumerate(instances.iterrows(), start=1):
        try:
            df, manifest = process_instance(row, repo_dir, work_root)
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
