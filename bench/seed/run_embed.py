"""임베딩 seed 산출 파이프라인.

    bench/.venv/bin/python -m bench.seed.run_embed \
        --add-repo pydata/xarray=<clone> --add-repo pytest-dev/pytest=<clone> \
        --features-csv bench/data/features_all.csv \
        --out-dir bench/data/seed_all

산출:
    embed_seed__<condition>.csv   # instance_id, file_id, embed, embed_rank
    embed_seed_long.csv           # + condition, aggregation (maxsim/maxpool)
    embed_blobs.npz               # blob SHA -> 청크 벡터 캐시 (재실행 시 재사용)

`run_seed.py`(BM25)와 같은 구조·같은 후보 집합·같은 순위 규칙을 쓴다.
그래야 두 의미 신호를 같은 조건에서 비교할 수 있다.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import pandas as pd

from bench.schema import C, DATA_DIR, normalize_file_id
from bench.seed import textproc
from bench.seed.corpus import MAX_BYTES, _batch_read, list_source_blobs
from bench.seed.dataset import load_instances
from bench.seed.embed import (AGGREGATIONS, DEFAULT_MODEL, MAXSIM, BlobCache,
                              ChunkedEncoder, score_file)
from bench.seed.run_seed import rank_pessimistic

SEED_COLUMNS = [C.instance_id, C.file_id, C.embed, C.embed_rank]


def run(repo_dirs: dict[str, Path], candidates: dict[str, set[str]],
        *, conditions: list[str], model: str = DEFAULT_MODEL,
        aggregations: tuple[str, ...] = AGGREGATIONS,
        cache_path: Path | None = None, batch_size: int = 256,
        autosave_every: int = 200,
        threads: int | None = None, out_dir: Path = DATA_DIR) -> dict:
    enc = ChunkedEncoder(model, batch_size=batch_size, threads=threads)
    cache = BlobCache.load(cache_path)
    cache.autosave_every = autosave_every
    instances = load_instances(instance_ids=list(candidates))

    rows: list[dict] = []
    ok, failed = 0, []
    t0 = time.time()

    for n, inst in enumerate(instances, 1):
        try:
            repo_dir = repo_dirs.get(inst.repo)
            if repo_dir is None:
                raise RuntimeError(f"{inst.repo} clone 경로 없음 (--add-repo)")
            cand = candidates[inst.instance_id]
            blobs = [(p, s) for p, s in list_source_blobs(repo_dir, inst.base_commit)
                     if p in cand]
            if not blobs:
                failed.append((inst.instance_id, "empty-corpus"))
                continue

            need = sorted({s for _, s in blobs if s not in cache.vecs})
            contents = _batch_read(repo_dir, need) if need else {}

            file_vecs: list[tuple[str, "object"]] = []
            for path, sha in blobs:
                if sha in cache.vecs:
                    file_vecs.append((path, cache.vecs[sha]))
                    enc.stats.cache_hits += 1
                    continue
                raw = contents.get(sha)
                if raw is None:
                    failed.append((f"{inst.instance_id}:{path}", "blob-missing"))
                    continue
                text = raw[:MAX_BYTES].decode("utf-8", errors="replace")
                v = enc.encode_text(text)
                cache.vecs[sha] = v
                file_vecs.append((path, v))

            for cond in conditions:
                q = enc.encode_query(textproc.make_query(inst.problem_statement, cond))
                for agg in aggregations:
                    scores = [score_file(q, v, agg) for _, v in file_vecs]
                    ranks = rank_pessimistic(scores)
                    for (fid, _), s, r in zip(file_vecs, scores, ranks):
                        rows.append({C.instance_id: inst.instance_id,
                                     C.file_id: fid, C.embed: s, C.embed_rank: r,
                                     "condition": cond, "aggregation": agg})
            ok += 1
        except Exception as e:  # 실패를 조용히 넘기지 않는다 (CONTRACT.md §3-7)
            failed.append((inst.instance_id, f"{type(e).__name__}: {e}"))

        if n % 10 == 0 or n == len(instances):
            print(f"  [{n}/{len(instances)}] ok={ok} failed={len(failed)} "
                  f"{enc.stats} {time.time() - t0:.0f}s", file=sys.stderr, flush=True)

    cache.save()
    out_dir.mkdir(parents=True, exist_ok=True)
    long_df = pd.DataFrame(rows)
    long_df.to_csv(out_dir / "embed_seed_long.csv", index=False)
    # 기본 집계(maxsim)만 조인용 파일로 낸다 — 근거는 embed.py 모듈 docstring
    primary = long_df[long_df["aggregation"] == MAXSIM]
    for cond in conditions:
        primary[primary["condition"] == cond][SEED_COLUMNS].to_csv(
            out_dir / f"embed_seed__{cond}.csv", index=False)

    return {"ok": ok, "failed": failed, "n_instances": len(instances),
            "n_rows": len(long_df), "stats": enc.stats, "out_dir": str(out_dir),
            "model": model}


def main(argv=None):
    ap = argparse.ArgumentParser(description="임베딩 seed 산출")
    ap.add_argument("--add-repo", action="append", default=[], metavar="REPO=DIR",
                    required=True)
    ap.add_argument("--features-csv", required=True, type=Path,
                    help="후보 집합의 출처. BM25 seed와 반드시 같은 것을 쓴다")
    ap.add_argument("--conditions", default=",".join(textproc.CONDITIONS))
    ap.add_argument("--model", default=DEFAULT_MODEL)
    ap.add_argument("--batch-size", type=int, default=256,
                    help="CPU에서는 크게 잡아야 스레드가 논다. 64는 병렬화가 안 걸린다")
    ap.add_argument("--threads", type=int, default=None)
    ap.add_argument("--autosave-every", type=int, default=200,
                    help="N개 blob마다 캐시 증분 저장. 0이면 끝에 한 번만")
    ap.add_argument("--cache", type=Path, default=None,
                    help="blob 벡터 캐시 npz. 기본 <out-dir>/embed_blobs.npz")
    ap.add_argument("--out-dir", type=Path, default=DATA_DIR)
    a = ap.parse_args(argv)

    repo_dirs: dict[str, Path] = {}
    for spec in a.add_repo:
        if "=" not in spec:
            raise SystemExit(f"--add-repo 형식은 REPO=DIR 이다: {spec!r}")
        r, d = spec.split("=", 1)
        repo_dirs[r] = Path(d)

    conds = [c.strip() for c in a.conditions.split(",") if c.strip()]
    for c in conds:
        if c not in textproc.CONDITIONS:
            raise SystemExit(f"unknown condition {c!r}")

    f = pd.read_csv(a.features_csv)
    f[C.file_id] = f[C.file_id].map(normalize_file_id)
    candidates = {k: set(v) for k, v in
                  f.groupby(C.instance_id)[C.file_id].apply(set).items()}

    res = run(repo_dirs, candidates, conditions=conds, model=a.model,
              cache_path=a.cache or (a.out_dir / "embed_blobs.npz"),
              autosave_every=a.autosave_every,
              batch_size=a.batch_size, threads=a.threads, out_dir=a.out_dir)

    print(f"\nmodel={res['model']}")
    print(f"instances={res['n_instances']} ok={res['ok']} failed={len(res['failed'])}")
    for iid, why in res["failed"][:20]:
        print(f"  FAIL {iid}: {why}")
    print(f"encode: {res['stats']}")
    print(f"rows={res['n_rows']}  ->  {res['out_dir']}")


if __name__ == "__main__":
    main()
