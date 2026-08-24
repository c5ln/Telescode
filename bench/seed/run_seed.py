"""BM25 seed 산출 파이프라인.

    python -m bench.seed.run_seed --repo pytest-dev/pytest \
        --repo-dir /path/to/pytest-clone --limit 119

산출:
    bench/data/bm25_seed__<condition>.csv   # instance_id, file_id, bm25, bm25_rank
    bench/data/bm25_seed_long.csv           # 위 전부 + condition (진단용)
    bench/data/bm25_leakage.csv             # 조건별 gold 경로 누출 진단

`--conditions`로 조건을 고른다. 계약상 **full 과 no_trace 는 항상 함께** 낸다.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import pandas as pd

from bench.schema import C, DATA_DIR, normalize_file_id
from bench.seed import textproc
from bench.seed.bm25 import BM25, B_DEFAULT, K1_DEFAULT, K3_DEFAULT
from bench.seed.corpus import build_corpus
from bench.seed.dataset import Instance, load_instances
from bench.seed.tokenizer import tokenize

SEED_COLUMNS = [C.instance_id, C.file_id, C.bm25, C.bm25_rank]


def rank_pessimistic(scores: list[float]) -> list[int]:
    """점수 내림차순 1-based 순위. 동점은 전부 그 그룹의 **최악** 순위를 받는다.

    라벨을 보지 않는다 — `bm25_rank`는 학습 피처이므로 gold를 참조하면 누출이다.
    (`bench.metrics.ranking.apply_pessimistic_tiebreak`은 gold를 보는 **평가용**이고,
    이건 라벨 없이 동점을 비관적으로 처리하는 **피처용**이다. 둘은 다른 물건이다.)
    """
    n = len(scores)
    order = sorted(range(n), key=lambda i: -scores[i])
    ranks = [0] * n
    i = 0
    while i < n:
        j = i
        while j + 1 < n and scores[order[j + 1]] == scores[order[i]]:
            j += 1
        worst = j + 1  # 1-based
        for k in range(i, j + 1):
            ranks[order[k]] = worst
        i = j + 1
    return ranks


def run(repos: list[tuple[str, Path]], *, conditions: list[str],
        limit: int | None = None, instance_ids: list[str] | None = None,
        k1: float = K1_DEFAULT, b: float = B_DEFAULT, k3: float | None = K3_DEFAULT,
        path_weight: int = 1,
        candidates: dict[str, set[str]] | None = None,
        out_dir: Path = DATA_DIR) -> dict:
    """여러 repo의 seed를 한 번에 낸다.

    BM25 인덱스는 **인스턴스마다 독립**이라(corpus = 그 커밋 시점 repo) repo를
    합쳐도 서로의 IDF에 영향을 주지 않는다. 단순 행 결합이며, 그래서 229 인스턴스
    통합 매트릭스에 그대로 조인된다.

    Args:
        repos: `(repo, clone_dir)` 목록. 인스턴스는 각 repo의 SWE-bench 항목에서
            나오고, 해당 repo의 clone에서만 corpus를 읽는다.
    """
    instances: list[tuple[Instance, Path]] = []
    for repo, repo_dir in repos:
        got = load_instances(repo=repo, instance_ids=instance_ids, limit=limit)
        if not got:
            raise SystemExit(f"인스턴스 0개. repo={repo!r} 확인 필요.")
        instances.extend((i, repo_dir) for i in got)

    rows: list[dict] = []
    leak_rows: list[dict] = []
    ok, failed = 0, []
    t0 = time.time()

    for n, (inst, repo_dir) in enumerate(instances, 1):
        try:
            cand = candidates.get(inst.instance_id) if candidates else None
            corp = build_corpus(repo_dir, inst.instance_id, inst.base_commit,
                                path_weight=path_weight, candidate_file_ids=cand)
            if not corp.doc_ids:
                failed.append((inst.instance_id, "empty-corpus"))
                continue

            index = BM25.build(corp.doc_ids, corp.docs, k1=k1, b=b, k3=k3)

            for cond in conditions:
                q = textproc.make_query(inst.problem_statement, cond)
                scores = index.score_query(tokenize(q))
                ranks = rank_pessimistic(scores)
                for fid, s, r in zip(corp.doc_ids, scores, ranks):
                    rows.append({C.instance_id: inst.instance_id,
                                 C.file_id: fid,
                                 C.bm25: s,
                                 C.bm25_rank: r,
                                 "condition": cond})

            rep = textproc.leakage_report(inst.problem_statement, set(inst.gold_files))
            for cond, v in rep.items():
                leak_rows.append({C.instance_id: inst.instance_id,
                                  "condition": cond,
                                  "n_docs": len(corp.doc_ids),
                                  "n_skipped": len(corp.skipped),
                                  "gold_in_corpus": sum(
                                      1 for g in inst.gold_files if g in set(corp.doc_ids)),
                                  **v})
            ok += 1
        except Exception as e:  # 실패를 조용히 넘기지 않는다 (CONTRACT.md §3-7)
            failed.append((inst.instance_id, f"{type(e).__name__}: {e}"))

        if n % 10 == 0 or n == len(instances):
            print(f"  [{n}/{len(instances)}] ok={ok} failed={len(failed)} "
                  f"{time.time() - t0:.0f}s", file=sys.stderr, flush=True)

    out_dir.mkdir(parents=True, exist_ok=True)
    long_df = pd.DataFrame(rows)
    long_df.to_csv(out_dir / "bm25_seed_long.csv", index=False)
    for cond in conditions:
        sub = long_df[long_df["condition"] == cond][SEED_COLUMNS]
        sub.to_csv(out_dir / f"bm25_seed__{cond}.csv", index=False)
    leak_df = pd.DataFrame(leak_rows)
    leak_df.to_csv(out_dir / "bm25_leakage.csv", index=False)

    return {"ok": ok, "failed": failed, "n_instances": len(instances),
            "n_rows": len(long_df), "long": long_df, "leak": leak_df,
            "out_dir": str(out_dir)}


def main(argv=None):
    ap = argparse.ArgumentParser(description="BM25 seed 산출")
    ap.add_argument("--repo", default=None,
                    help="단일 repo. --repo-dir와 함께 쓴다")
    ap.add_argument("--repo-dir", type=Path, default=None)
    ap.add_argument("--add-repo", action="append", default=[], metavar="REPO=DIR",
                    help="여러 repo를 한 번에. 반복 지정 가능. "
                         "예: --add-repo pydata/xarray=/c/xarray "
                         "--add-repo pytest-dev/pytest=/c/pytest")
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--conditions", default=",".join(textproc.CONDITIONS))
    ap.add_argument("--k1", type=float, default=K1_DEFAULT)
    ap.add_argument("--b", type=float, default=B_DEFAULT)
    ap.add_argument("--k3", default=K3_DEFAULT,
                    type=lambda v: None if v.lower() in ("none", "linear") else float(v),
                    help="None/linear = 선형(기본, 표준). 0 = 이진. 그 외 = 포화 계수")
    ap.add_argument("--path-weight", type=int, default=1)
    ap.add_argument("--features-csv", type=Path, default=None,
                    help="주면 그 매트릭스의 file_id로 corpus를 제한한다")
    ap.add_argument("--out-dir", type=Path, default=DATA_DIR)
    a = ap.parse_args(argv)

    conds = [c.strip() for c in a.conditions.split(",") if c.strip()]
    for c in conds:
        if c not in textproc.CONDITIONS:
            raise SystemExit(f"unknown condition {c!r}")
    if textproc.FULL not in conds or textproc.NO_TRACE not in conds:
        print("WARN: 계약상 full 과 no_trace 는 항상 함께 보고해야 한다.",
              file=sys.stderr)

    candidates = None
    if not a.features_csv:
        # 후보 집합이 없으면 seed가 스캔에 없는 file_id를 뱉어도 아무도 못 잡는다.
        # 조인 커버리지 검사가 어디서도 안 걸리는 경로라 경고를 띄운다.
        # (bench-harness가 팀리드 지시로 추가 — 감사 S2-2)
        print("WARN: --features-csv 없이 실행하면 후보 집합 제한과 조인 커버리지 "
              "검증이 모두 생략된다. 산출된 seed는 bench/features/join_bm25.py 로 "
              "반드시 검증할 것.", file=sys.stderr)
    if a.features_csv:
        f = pd.read_csv(a.features_csv)
        f[C.file_id] = f[C.file_id].map(normalize_file_id)
        candidates = {k: set(v) for k, v in
                      f.groupby(C.instance_id)[C.file_id].apply(set).items()}

    repos: list[tuple[str, Path]] = []
    for spec in a.add_repo:
        if "=" not in spec:
            raise SystemExit(f"--add-repo 형식은 REPO=DIR 이다: {spec!r}")
        r, d = spec.split("=", 1)
        repos.append((r, Path(d)))
    if a.repo or a.repo_dir:
        if not (a.repo and a.repo_dir):
            raise SystemExit("--repo 와 --repo-dir 는 함께 준다")
        repos.append((a.repo, a.repo_dir))
    if not repos:
        raise SystemExit("--repo/--repo-dir 또는 --add-repo 중 하나는 필요하다")

    res = run(repos, conditions=conds, limit=a.limit,
              k1=a.k1, b=a.b, k3=a.k3, path_weight=a.path_weight,
              candidates=candidates, out_dir=a.out_dir)
    print(f"\nrepos={[r for r, _ in repos]}")
    print(f"instances={res['n_instances']} ok={res['ok']} failed={len(res['failed'])}")
    for iid, why in res["failed"]:
        print(f"  FAIL {iid}: {why}")
    print(f"rows={res['n_rows']}  ->  {res['out_dir']}")


if __name__ == "__main__":
    main()
