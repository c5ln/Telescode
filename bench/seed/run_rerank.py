"""리랭킹 seed 산출 파이프라인.

    bench/.venv/bin/python -m bench.seed.run_rerank \
        --add-repo pydata/xarray=bench/repos/xarray \
        --add-repo pytest-dev/pytest=bench/repos/pytest \
        --embed-seed bench/data/seed_all/embed_seed__no_paths.csv \
        --cache bench/data/seed_all/embed_blobs.npz \
        --out-dir bench/data/seed_all

산출:
    rerank_seed__<condition>.csv   # instance_id, file_id, rerank, rerank_rank

## 절차

    1. 인스턴스별로 `embed` 상위 N개 파일을 고른다
    2. 각 파일에서 **쿼리와 가장 가까운 청크 하나**를 고른다
       - 캐시된 blob 벡터(`embed_blobs.npz`)로 argmax를 구한다
       - 그 인덱스의 청크 텍스트는 파일을 다시 잘라서 얻는다 (결정론적·무료)
    3. (이슈 텍스트, 청크 N개)를 크로스 인코더에 넘긴다
    4. relevance를 `rerank`, 비관적 순위를 `rerank_rank`로 낸다

2단계의 argmax를 구하려면 쿼리 벡터가 필요한데 임베딩 단계에서 저장하지 않았다.
229개를 다시 인코딩한다 — 약 $0.01이라 저장 구조를 바꾸는 것보다 싸다.

## 후보 밖 파일은 NaN이다

리랭킹하지 않은 파일에 0을 넣으면 "관련 없음"을 **측정했다**는 뜻이 되어 버린다.
측정하지 않은 것과 0점을 받은 것은 다르므로 NaN으로 둔다. LTR은 결측을 그대로
다루고, 랭킹 지표에서는 `ranking.apply_pessimistic_tiebreak`가 NaN을 최하위로 보낸다.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import numpy as np
import pandas as pd

from bench.schema import C, DATA_DIR, normalize_file_id
from bench.seed import textproc
from bench.seed.api_encoder import OpenRouterEncoder
from bench.seed.corpus import MAX_BYTES, _batch_read, list_source_blobs
from bench.seed.dataset import load_instances
from bench.seed.embed import BlobCache, encoder_fingerprint
from bench.seed.rerank import TOP_N, OpenRouterReranker
from bench.seed.run_seed import rank_pessimistic

SEED_COLUMNS = [C.instance_id, C.file_id, C.rerank, C.rerank_rank]


def build_jobs(enc, cache: BlobCache, repo_dirs: dict[str, Path],
               embed_seed: pd.DataFrame, instances, *, condition: str,
               top_n: int, with_path: bool = True, dry_run: bool = False):
    """인스턴스별 (쿼리, 문서 N개)와 그 문서가 어느 파일인지를 만든다."""
    by_inst = dict(tuple(embed_seed.groupby(C.instance_id)))
    jobs, owners, skipped = [], [], []
    n_chars = 0

    for n, inst in enumerate(instances, 1):
        g = by_inst.get(inst.instance_id)
        if g is None or g.empty:
            skipped.append((inst.instance_id, "embed seed 없음"))
            continue
        top = g.nlargest(top_n, C.embed)
        want = set(top[C.file_id])

        repo_dir = repo_dirs[inst.repo]
        blobs = {p: s for p, s in list_source_blobs(repo_dir, inst.base_commit)
                 if p in want}
        contents = _batch_read(repo_dir, sorted(set(blobs.values())))

        query = textproc.make_query(inst.problem_statement, condition)
        qv = None if dry_run else enc.encode_query(query)

        docs, files = [], []
        for path in top[C.file_id]:
            sha = blobs.get(path)
            raw = contents.get(sha) if sha else None
            if raw is None:
                continue
            text = raw[:MAX_BYTES].decode("utf-8", errors="replace")
            chunks = enc.chunk_texts(text)
            if not chunks:
                continue
            if dry_run:
                pick = chunks[0]
            else:
                vecs = cache.vecs.get(sha)
                if vecs is None or len(vecs) == 0:
                    continue
                # 캐시 벡터와 방금 자른 청크 수가 어긋나면 청크 파라미터가
                # 캐시 생성 시점과 달라진 것이다. 조용히 밀리면 다른 파일의
                # 조각을 리랭커에 보내게 되므로 여기서 막는다.
                if len(vecs) != len(chunks):
                    raise RuntimeError(
                        f"{inst.instance_id}:{path} 청크 수 불일치 "
                        f"(캐시 {len(vecs)} vs 재분할 {len(chunks)}). "
                        "인코더 지문과 청크 파라미터를 확인하라")
                pick = chunks[int(np.argmax(vecs @ qv))]
            # **파일 경로를 문서 앞에 붙인다.** 없으면 리랭커는 어느 파일에서
            # 온 조각인지 모른 채 코드만 보고 판단한다. 실측(3 인스턴스)에서
            # 경로를 붙이자 시험한 4개 모델 전부가 올랐다 (+0.045 ~ +0.264).
            # `no_paths` 조건은 **쿼리**에서 경로를 지운 것이므로 누출이 아니다.
            docs.append(f"File: {path}\n\n{pick}" if with_path else pick)
            files.append(path)
            n_chars += len(pick) + (len(path) + 9 if with_path else 0)

        if not docs:
            skipped.append((inst.instance_id, "문서 없음"))
            continue
        jobs.append((query, docs))
        owners.append((inst.instance_id, files))
        n_chars += len(query)

        if n % 25 == 0 or n == len(instances):
            print(f"  build [{n}/{len(instances)}] jobs={len(jobs)} "
                  f"chars={n_chars:,}", file=sys.stderr, flush=True)

    return jobs, owners, skipped, n_chars


def run(repo_dirs: dict[str, Path], *, embed_seed_path: Path, cache_path: Path,
        condition: str, top_n: int = TOP_N, model: str | None = None,
        with_path: bool = True,
        embed_model: str | None = None, dimensions: int = 1024,
        concurrency: int = 8, dry_run: bool = False,
        out_dir: Path = DATA_DIR) -> dict:
    enc_kw = {"dimensions": dimensions}
    if embed_model:
        enc_kw["model"] = embed_model
    enc = OpenRouterEncoder(**enc_kw) if embed_model else OpenRouterEncoder(
        dimensions=dimensions)

    cache = BlobCache.load(cache_path, fingerprint=encoder_fingerprint(enc))
    print(f"blob 캐시 {len(cache.vecs):,}개 (지문 일치)", file=sys.stderr)

    seed = pd.read_csv(embed_seed_path)
    seed[C.file_id] = seed[C.file_id].map(normalize_file_id)
    instances = load_instances(instance_ids=list(seed[C.instance_id].unique()))

    t0 = time.time()
    jobs, owners, skipped, n_chars = build_jobs(
        enc, cache, repo_dirs, seed, instances,
        condition=condition, top_n=top_n, with_path=with_path, dry_run=dry_run)

    if dry_run:
        return {"dry_run": True, "n_jobs": len(jobs),
                "n_docs": sum(len(d) for _, d in jobs), "n_chars": n_chars,
                "skipped": skipped, "seconds": time.time() - t0}

    rr = OpenRouterReranker(model or "voyageai/rerank-2.5", concurrency=concurrency)
    print(f"리랭킹 {len(jobs)}개 인스턴스 × 평균 "
          f"{sum(len(d) for _, d in jobs) / max(len(jobs), 1):.0f}개 문서",
          file=sys.stderr, flush=True)
    scores = rr.rerank_batch(jobs)

    rows = []
    for (iid, files), sc in zip(owners, scores):
        ranks = rank_pessimistic(list(sc))
        for fid, s, r in zip(files, sc, ranks):
            rows.append({C.instance_id: iid, C.file_id: fid,
                         C.rerank: float(s), C.rerank_rank: int(r)})

    out_dir.mkdir(parents=True, exist_ok=True)
    df = pd.DataFrame(rows)
    df[SEED_COLUMNS].to_csv(out_dir / f"rerank_seed__{condition}.csv", index=False)
    return {"n_jobs": len(jobs), "n_rows": len(df), "skipped": skipped,
            "stats": rr.stats, "model": rr.model,
            "out": str(out_dir / f"rerank_seed__{condition}.csv")}


def main(argv=None):
    ap = argparse.ArgumentParser(description="리랭킹 seed 산출")
    ap.add_argument("--add-repo", action="append", default=[], metavar="REPO=DIR",
                    required=True)
    ap.add_argument("--embed-seed", required=True, type=Path,
                    help="후보 선택의 출처. 조건이 --condition과 같아야 한다")
    ap.add_argument("--cache", required=True, type=Path,
                    help="임베딩 blob 벡터 캐시 npz (최적 청크 선택에 쓴다)")
    ap.add_argument("--condition", default=textproc.NO_PATHS)
    ap.add_argument("--top-n", type=int, default=TOP_N)
    ap.add_argument("--model", default="cohere/rerank-4-fast")
    ap.add_argument("--no-path", action="store_true",
                    help="문서에 파일 경로를 붙이지 않는다 (기본은 붙인다)")
    ap.add_argument("--dimensions", type=int, default=1024)
    ap.add_argument("--concurrency", type=int, default=8)
    ap.add_argument("--dry-run", action="store_true",
                    help="API 호출 없이 문서 수·문자 수만 센다. 비용 확인용")
    ap.add_argument("--out-dir", type=Path, default=DATA_DIR)
    a = ap.parse_args(argv)

    repo_dirs: dict[str, Path] = {}
    for spec in a.add_repo:
        if "=" not in spec:
            raise SystemExit(f"--add-repo 형식은 REPO=DIR 이다: {spec!r}")
        r, d = spec.split("=", 1)
        repo_dirs[r] = Path(d)

    res = run(repo_dirs, embed_seed_path=a.embed_seed, cache_path=a.cache,
              condition=a.condition, top_n=a.top_n, model=a.model,
              with_path=not a.no_path,
              dimensions=a.dimensions, concurrency=a.concurrency,
              dry_run=a.dry_run, out_dir=a.out_dir)

    if res.get("dry_run"):
        tok = res["n_chars"] / 3.5
        print(f"\nDRY RUN — 인스턴스 {res['n_jobs']} · 문서 {res['n_docs']:,} · "
              f"문자 {res['n_chars']:,}")
        print(f"  추정 토큰 {tok:,.0f}  (voyage rerank-2.5 $0.05/1M 기준 "
              f"≈ ${tok / 1e6 * 0.05:.2f})")
    else:
        print(f"\nmodel={res['model']}")
        print(f"인스턴스 {res['n_jobs']} · 행 {res['n_rows']:,}")
        print(f"{res['stats']}")
        print(f"-> {res['out']}")
    for iid, why in res["skipped"][:20]:
        print(f"  SKIP {iid}: {why}")


if __name__ == "__main__":
    main()
