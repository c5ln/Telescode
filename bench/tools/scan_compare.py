"""두 바이너리의 새 스캔 비교 — 스캔 시간, DB 크기, 결과 표(특히 reading_sequence 비트).

    bench/.venv/bin/python -m bench.tools.scan_compare --old <bin> --new <bin> \\
        --repo xarray=bench/repos/xarray --repo root=. --work /tmp/sc --out x.json

저장소마다 `.py` 파일을 작업 디렉터리로 복사해 두 바이너리로 각각 `scan` + `algo`하고
`update_equiv.compare_dbs`로 비교한다(A = old, B = new). 스키마/user_version 차이는
버전이 다르면 당연하므로 `differs`에서 빼고 `schema_differs`로만 남긴다. 스캔 시간은
`--reps`번 중 최솟값. DB는 비교 직후 지운다.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

from bench.tools.update_equiv import ROOT, _md5, _rm_db, compact, compare_dbs, copy_py_tree, scan


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--old", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("--repo", action="append", required=True, help="name=path")
    ap.add_argument("--work", required=True)
    ap.add_argument("--out")
    ap.add_argument("--reps", type=int, default=1)
    args = ap.parse_args(argv)

    work = Path(args.work).resolve()
    bins = {"old": str(Path(args.old).resolve()), "new": str(Path(args.new).resolve())}
    results = []
    for kv in args.repo:
        name, _, path = kv.partition("=")
        tree = work / "tree"
        if tree.exists():
            shutil.rmtree(tree)
        tree.mkdir(parents=True)
        n_files = copy_py_tree((ROOT / path).resolve(), tree)
        dbs = {k: work / f"{k}.db" for k in bins}
        r = {"repo": name, "files": n_files}
        try:
            for k, b in bins.items():
                times = [scan(b, tree, dbs[k]) for _ in range(args.reps)]
                r[f"{k}_scan_ms"] = round(min(t[0] for t in times) * 1000, 1)
                r[f"{k}_algo_ms"] = round(min(t[1] for t in times) * 1000, 1)
                r[f"{k}_db_bytes"] = dbs[k].stat().st_size
            rep = compact(compare_dbs(dbs["old"], dbs["new"]))
        finally:
            for db in dbs.values():
                _rm_db(db)
            shutil.rmtree(tree, ignore_errors=True)
        rep["differs"] = [t for t in rep["differs"] if t not in ("schema", "user_version")]
        rep["equal"] = not rep["differs"]
        r["compare"] = rep
        results.append(r)
        print(f"{name}: files {n_files}, scan {r['old_scan_ms']:.0f} -> {r['new_scan_ms']:.0f} ms,"
              f" algo {r['old_algo_ms']:.0f} -> {r['new_algo_ms']:.0f} ms,"
              f" db {r['old_db_bytes'] / 2**20:.1f} -> {r['new_db_bytes'] / 2**20:.1f} MB,"
              f" equal {rep['equal']} {rep['differs']}", flush=True)
    report = {"old": bins["old"], "old_md5": _md5(bins["old"]), "new": bins["new"],
              "new_md5": _md5(bins["new"]), "repos": results}
    if args.out:
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(json.dumps(report, indent=1, ensure_ascii=False) + "\n",
                                  encoding="utf-8")
    return 0 if all(r["compare"]["equal"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
