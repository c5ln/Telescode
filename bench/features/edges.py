"""파일 단위 간선 재추출 — 이웃 BM25 피처(`code_lexical.nbr_bm25_*`)용.

`collect/pipeline.py`는 §6에 따라 DB를 추출 직후 지우므로 간선이 어디에도 남아
있지 않다. PageRank가 돈 그래프와 **같은 간선 집합**이 필요하므로
(`extract._file_level_edges`) base_commit 트리를 스캐너로만 다시 훑는다.
`TelescodeAlgo`는 link 테이블을 바꾸지 않으므로 돌리지 않는다.

같은 그래프인지는 추정하지 않고 **검증한다**: 다시 뽑은 간선으로 센 in_deg/out_deg와
파일 집합이 기존 매트릭스와 인스턴스 전 파일에서 정확히 같아야 한다. 하나라도
어긋나면 그 인스턴스는 실패로 기록하고 산출에서 뺀다 (§7).

    bench/.venv/bin/python -m bench.features.edges \
        --features bench/data/features_all_with_bm25__no_paths.csv \
        --manifest bench/data/instances_all.csv \
        --add-repo pydata/xarray=bench/repos/xarray \
        --add-repo pytest-dev/pytest=bench/repos/pytest \
        --out bench/data/file_edges_all.csv
"""

from __future__ import annotations

import argparse
import shutil
import sqlite3
import sys
import tempfile
import time
from collections import Counter
from pathlib import Path

import pandas as pd

from bench.collect.pipeline import InstanceFailure, _export_tree, _remove_db, _run
from bench.features.extract import _file_level_edges
from bench.schema import C, SCANNER_BIN, SCRATCH_DIR

EDGE_COLUMNS = [C.instance_id, "src", "tgt"]


def repo_of(instance_id: str) -> str:
    """`pydata__xarray-3095` → `pydata/xarray`."""
    owner, rest = instance_id.split("__", 1)
    return f"{owner}/{rest.rsplit('-', 1)[0]}"


def scan_edges(repo_dir: Path, instance_id: str, commit: str, work_root: Path,
               timeout: int = 900) -> tuple[set[str], set[tuple[str, str]]]:
    """base_commit 트리를 스캔해 (파일 집합, 간선 집합)을 돌려준다. DB·트리는 즉시 삭제."""
    tree = work_root / instance_id
    db = SCRATCH_DIR / f"{instance_id}.edges.db"
    shutil.rmtree(tree, ignore_errors=True)
    _remove_db(db)
    try:
        _export_tree(repo_dir, commit, tree, timeout)
        scan = _run([str(SCANNER_BIN), str(tree), str(db)], timeout)
        if scan.returncode != 0:
            raise InstanceFailure(f"scanner rc={scan.returncode}: {scan.stderr.strip()[:200]}")
        if not db.exists():
            raise InstanceFailure("scanner가 DB를 만들지 않았다")
        conn = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
        try:
            file_ids = {r[0] for r in conn.execute("SELECT file_id FROM file")}
            return file_ids, _file_level_edges(conn, file_ids)
        finally:
            conn.close()
    finally:
        _remove_db(db)
        shutil.rmtree(tree, ignore_errors=True)


def degree_mismatches(edges: set[tuple[str, str]], rows: pd.DataFrame) -> int:
    """간선으로 다시 센 차수가 매트릭스의 in_deg/out_deg와 다른 행 수."""
    out_deg = Counter(s for s, _ in edges)
    in_deg = Counter(t for _, t in edges)
    exp_out = rows[C.file_id].map(out_deg).fillna(0).astype(int)
    exp_in = rows[C.file_id].map(in_deg).fillna(0).astype(int)
    bad = (exp_out.to_numpy() != rows[C.out_deg].to_numpy()) | \
          (exp_in.to_numpy() != rows[C.in_deg].to_numpy())
    return int(bad.sum())


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--features", required=True, help="검증 기준 매트릭스 (in_deg/out_deg)")
    ap.add_argument("--manifest", required=True, help="base_commit 출처")
    ap.add_argument("--add-repo", action="append", default=[], metavar="REPO=DIR")
    ap.add_argument("--work-root", default=None, help="트리를 펼칠 임시 디렉토리")
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)

    if not SCANNER_BIN.exists():
        print(f"pin된 바이너리 없음: {SCANNER_BIN}", file=sys.stderr)
        return 2
    repos = {}
    for spec in args.add_repo:
        if "=" not in spec:
            raise SystemExit(f"--add-repo 형식은 REPO=DIR 이다: {spec!r}")
        r, d = spec.split("=", 1)
        repos[r] = Path(d).resolve()

    features = pd.read_csv(args.features,
                           usecols=[C.instance_id, C.file_id, C.in_deg, C.out_deg])
    by_inst = dict(tuple(features.groupby(C.instance_id)))
    manifest = pd.read_csv(args.manifest)
    manifest = manifest[manifest["status"] == "ok"]

    SCRATCH_DIR.mkdir(parents=True, exist_ok=True)
    work_root = Path(args.work_root) if args.work_root else Path(
        tempfile.mkdtemp(prefix="bench-edges-"))
    work_root.mkdir(parents=True, exist_ok=True)

    rows, failed = [], []
    t0 = time.monotonic()
    for n, r in enumerate(manifest.itertuples(index=False), 1):
        iid = r.instance_id
        try:
            repo = repo_of(iid)
            if repo not in repos:
                raise InstanceFailure(f"--add-repo에 {repo}가 없다")
            if iid not in by_inst:
                raise InstanceFailure("매트릭스에 이 인스턴스가 없다")
            file_ids, edges = scan_edges(repos[repo], iid, r.base_commit, work_root)
            mat = by_inst[iid]
            if file_ids != set(mat[C.file_id]):
                raise InstanceFailure(
                    f"파일 집합 불일치: 스캔 {len(file_ids)} / 매트릭스 {len(mat)}")
            bad = degree_mismatches(edges, mat)
            if bad:
                raise InstanceFailure(f"차수 불일치 {bad}행 — PageRank 그래프와 다른 간선")
            rows.extend({C.instance_id: iid, "src": s, "tgt": t} for s, t in sorted(edges))
        except Exception as exc:                      # noqa: BLE001 — §7
            failed.append((iid, f"{type(exc).__name__}: {exc}"[:300]))
        if n % 20 == 0 or n == len(manifest):
            print(f"  [{n}/{len(manifest)}] ok={n - len(failed)} failed={len(failed)} "
                  f"{time.monotonic() - t0:.0f}s", flush=True)

    shutil.rmtree(work_root, ignore_errors=True)
    pd.DataFrame(rows, columns=EDGE_COLUMNS).to_csv(args.out, index=False)
    print(f"간선 {len(rows)}개 → {args.out}")
    print(f"성공 {len(manifest) - len(failed)} / 실패 {len(failed)} / 전체 {len(manifest)}")
    for iid, why in failed:
        print(f"  FAIL {iid}: {why}", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
