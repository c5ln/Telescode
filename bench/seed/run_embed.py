"""임베딩 seed 산출 파이프라인.

    bench/.venv/bin/python -m bench.seed.run_embed \
        --add-repo pydata/xarray=bench/repos/xarray \
        --add-repo pytest-dev/pytest=bench/repos/pytest \
        --features-csv bench/data/features_all.csv \
        --out-dir bench/data/seed_all

산출:
    embed_seed__<condition>.csv   # instance_id, file_id, embed, embed_rank
    embed_seed_long.csv           # + condition, aggregation (maxsim/maxpool)
    embed_blobs.npz               # blob SHA -> 청크 벡터 캐시 (재실행 시 재사용)

`run_seed.py`(BM25)와 같은 구조·같은 후보 집합·같은 순위 규칙을 쓴다.
그래야 두 의미 신호를 같은 조건에서 비교할 수 있다.

## 2단계 구조 (중요)

    1단계  코퍼스 인코딩 : 전 인스턴스의 후보를 blob SHA로 합쳐 **한 번만** 인코딩
    2단계  인스턴스 채점 : 캐시된 벡터로 순수 numpy 연산

예전에는 인스턴스 루프 **안에서** 파일을 인코딩했다. 두 가지가 잘못됐다.

  1. 배치가 파일 단위(청크 2~10개)로 쪼개져 `--batch-size`가 무의미했다.
     API 경로에서는 이게 곧 왕복 지연 지배를 뜻한다.
  2. (instance, file) 43,688쌍 중 고유 blob은 6,308개다 — **6.93배 중복**.
     인스턴스별로 돌면 이 중복이 캐시 적중으로만 흡수되는데, 캐시 적중 판정이
     루프 안에 있어서 배치를 미리 채울 수 없다.

코퍼스를 먼저 훑으면 배치가 꽉 차고, 진행률과 남은 비용을 미리 알 수 있다.
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
from bench.seed.corpus import MAX_BYTES, _batch_read, list_source_blobs
from bench.seed.dataset import load_instances
from bench.seed.embed import (AGGREGATIONS, MAXSIM, BlobCache, encoder_fingerprint,
                              score_file)
from bench.seed.run_seed import rank_pessimistic

SEED_COLUMNS = [C.instance_id, C.file_id, C.embed, C.embed_rank]

#: 코퍼스 인코딩 시 한 번에 디스크에서 읽어올 blob 수. git cat-file 배치 크기다.
READ_BATCH = 400


def plan_corpus(repo_dirs: dict[str, Path], candidates: dict[str, set[str]],
                instances) -> tuple[dict[str, Path], list[tuple[str, str, Path]]]:
    """전 인스턴스의 후보 파일을 blob SHA로 합친다.

    Returns:
        (sha -> repo_dir, [(instance_id, path, sha)] 형태의 소속 목록)
        두 번째 값은 2단계에서 인스턴스별 파일 목록을 되살리는 데 쓴다.
    """
    sha_repo: dict[str, Path] = {}
    membership: list[tuple[str, str, Path]] = []
    for inst in instances:
        repo_dir = repo_dirs.get(inst.repo)
        if repo_dir is None:
            raise RuntimeError(f"{inst.repo} clone 경로 없음 (--add-repo)")
        cand = candidates[inst.instance_id]
        for path, sha in list_source_blobs(repo_dir, inst.base_commit):
            if path in cand:
                sha_repo.setdefault(sha, repo_dir)
                membership.append((inst.instance_id, path, sha))
    return sha_repo, membership


def encode_corpus(enc, cache: BlobCache, sha_repo: dict[str, Path],
                  *, dry_run: bool = False) -> dict:
    """1단계. 캐시에 없는 blob만 청크로 잘라 **파일 경계를 넘어 배치로** 인코딩한다.

    청크를 파일별로 쪼개 보내지 않는 것이 핵심이다. 여러 파일의 청크를 한 리스트로
    모아 `enc.embed_texts`에 넘기고, 돌아온 행을 파일별로 다시 나눠 담는다.

    Args:
        dry_run: 인코딩하지 않고 청크·토큰 수만 센다. **돈 쓰기 전 비용 확인용.**
    """
    need = sorted(s for s in sha_repo if s not in cache.vecs)
    stats = {"n_blobs": len(sha_repo), "n_cached": len(sha_repo) - len(need),
             "n_need": len(need), "n_chunks": 0, "n_chars": 0, "n_empty": 0}
    if not need:
        return stats

    by_repo: dict[Path, list[str]] = {}
    for sha in need:
        by_repo.setdefault(sha_repo[sha], []).append(sha)

    t0 = time.time()
    done = 0
    for repo_dir, shas in by_repo.items():
        for i in range(0, len(shas), READ_BATCH):
            group = shas[i:i + READ_BATCH]
            contents = _batch_read(repo_dir, group)

            # 이 그룹의 모든 청크를 한 리스트로 모은다 (파일 경계를 넘어서).
            flat: list[str] = []
            spans: list[tuple[str, int, int]] = []   # (sha, start, end)
            empty: list[str] = []
            for sha in group:
                raw = contents.get(sha)
                if raw is None:
                    continue
                text = raw[:MAX_BYTES].decode("utf-8", errors="replace")
                chunks = enc.chunk_texts(text)
                if not chunks:
                    # 빈 파일(내용 없는 __init__.py 등)은 **실패가 아니다.**
                    # 길이 0 배열을 넣어야 score_file이 0.0을 내고, 채점 단계가
                    # 이걸 "벡터 없음"(=버그)과 구분할 수 있다.
                    stats["n_empty"] += 1
                    empty.append(sha)
                    continue
                spans.append((sha, len(flat), len(flat) + len(chunks)))
                flat.extend(chunks)

            stats["n_chunks"] += len(flat)
            stats["n_chars"] += sum(len(c) for c in flat)
            done += len(group)

            if dry_run:
                _progress(done, len(need), stats, t0)
                continue

            vecs = enc.embed_texts(flat)
            for sha, a, b in spans:
                cache.vecs[sha] = vecs[a:b]
                cache.bump()
            for sha in empty:
                cache.vecs[sha] = np.zeros((0, enc.dim or 0), dtype=np.float32)
                cache.bump()
            _progress(done, len(need), stats, t0)

    if not dry_run:
        cache.save()
    stats["seconds"] = time.time() - t0
    return stats


def _progress(done: int, total: int, stats: dict, t0: float) -> None:
    el = time.time() - t0
    rate = done / el if el > 0 else 0
    eta = (total - done) / rate if rate > 0 else 0
    print(f"  encode [{done}/{total}] chunks={stats['n_chunks']:,} "
          f"{el:.0f}s eta={eta:.0f}s", file=sys.stderr, flush=True)


def score_instances(enc, cache: BlobCache, membership, instances,
                    *, conditions: list[str],
                    aggregations: tuple[str, ...] = AGGREGATIONS) -> tuple[list[dict], int, list]:
    """2단계. 캐시된 벡터로 인스턴스별 점수를 낸다. API 호출은 쿼리 인코딩뿐이다."""
    files_of: dict[str, list[tuple[str, str]]] = {}
    for iid, path, sha in membership:
        files_of.setdefault(iid, []).append((path, sha))

    rows: list[dict] = []
    ok, failed = 0, []
    t0 = time.time()
    for n, inst in enumerate(instances, 1):
        try:
            pairs = files_of.get(inst.instance_id, [])
            file_vecs = [(p, cache.vecs[s]) for p, s in pairs if s in cache.vecs]
            missing = len(pairs) - len(file_vecs)
            if missing:
                # 여기 걸리면 진짜 버그다. 빈 파일은 encode_corpus가 길이 0
                # 배열로 넣어두므로 정상 경로에서는 0이어야 한다.
                raise RuntimeError(f"{missing}/{len(pairs)}개 blob 벡터 누락")
            if not file_vecs:
                failed.append((inst.instance_id, "empty-corpus"))
                continue

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
        except Exception as e:
            failed.append((inst.instance_id, f"{type(e).__name__}: {e}"))

        if n % 25 == 0 or n == len(instances):
            print(f"  score [{n}/{len(instances)}] ok={ok} failed={len(failed)} "
                  f"{time.time() - t0:.0f}s", file=sys.stderr, flush=True)
    return rows, ok, failed


def run(repo_dirs: dict[str, Path], candidates: dict[str, set[str]],
        *, conditions: list[str], enc,
        aggregations: tuple[str, ...] = AGGREGATIONS,
        cache_path: Path | None = None, autosave_every: int = 200,
        limit: int | None = None, dry_run: bool = False,
        out_dir: Path = DATA_DIR) -> dict:
    fp = encoder_fingerprint(enc)
    cache = BlobCache.load(cache_path, fingerprint=fp)
    cache.fingerprint = fp
    cache.autosave_every = autosave_every

    instances = load_instances(instance_ids=list(candidates))
    if limit:
        instances = instances[:limit]

    sha_repo, membership = plan_corpus(repo_dirs, candidates, instances)
    enc_stats = encode_corpus(enc, cache, sha_repo, dry_run=dry_run)
    if dry_run:
        return {"dry_run": True, "corpus": enc_stats, "fingerprint": fp,
                "n_instances": len(instances)}

    rows, ok, failed = score_instances(enc, cache, membership, instances,
                                       conditions=conditions, aggregations=aggregations)

    out_dir.mkdir(parents=True, exist_ok=True)
    long_df = pd.DataFrame(rows)
    long_df.to_csv(out_dir / "embed_seed_long.csv", index=False)
    # 기본 집계(maxsim)만 조인용 파일로 낸다 — 근거는 embed.py 모듈 docstring
    primary = long_df[long_df["aggregation"] == MAXSIM]
    for cond in conditions:
        primary[primary["condition"] == cond][SEED_COLUMNS].to_csv(
            out_dir / f"embed_seed__{cond}.csv", index=False)

    return {"ok": ok, "failed": failed, "n_instances": len(instances),
            "n_rows": len(long_df), "stats": enc.stats, "corpus": enc_stats,
            "out_dir": str(out_dir), "fingerprint": fp}


def build_encoder(a):
    """`--backend`에 따라 인코더를 만든다.

    **자동 폴백은 없다.** API가 실패했을 때 로컬 모델로 넘어가면 한 실행 안에서
    두 모델의 벡터가 섞이고, 그 섞임은 예외를 내지 않는다 (`BlobCache` §지문).
    실패하면 실패한 채로 멈춘다.
    """
    if a.backend == "openrouter":
        from bench.seed.api_encoder import OpenRouterEncoder
        return OpenRouterEncoder(a.model, dimensions=a.dimensions,
                                 max_len=a.max_len, stride=a.stride,
                                 max_chunks=a.max_chunks, batch=a.batch_size,
                                 concurrency=a.concurrency)
    if a.backend == "local":
        from bench.seed.embed import ChunkedEncoder
        return ChunkedEncoder(a.model, max_len=a.max_len, stride=a.stride,
                              max_chunks=a.max_chunks, batch_size=a.batch_size,
                              threads=a.threads)
    raise SystemExit(f"unknown backend {a.backend!r}")


def main(argv=None):
    from bench.seed.api_encoder import (API_CONCURRENCY, API_DIMENSIONS,
                                        API_MAX_CHUNKS, API_MAX_LEN, API_STRIDE,
                                        DEFAULT_API_MODEL)

    ap = argparse.ArgumentParser(description="임베딩 seed 산출")
    ap.add_argument("--add-repo", action="append", default=[], metavar="REPO=DIR",
                    required=True)
    ap.add_argument("--features-csv", required=True, type=Path,
                    help="후보 집합의 출처. BM25 seed와 반드시 같은 것을 쓴다")
    ap.add_argument("--conditions", default=",".join(textproc.CONDITIONS))
    ap.add_argument("--backend", default="openrouter", choices=["openrouter", "local"],
                    help="local은 이 머신에서 5일 걸린다 (api_encoder.py 모듈 docstring)")
    ap.add_argument("--model", default=DEFAULT_API_MODEL)
    ap.add_argument("--dimensions", type=int, default=API_DIMENSIONS,
                    help="MRL 절단 차원. openrouter 백엔드 전용")
    ap.add_argument("--max-len", type=int, default=API_MAX_LEN)
    ap.add_argument("--stride", type=int, default=API_STRIDE)
    ap.add_argument("--max-chunks", type=int, default=API_MAX_CHUNKS)
    ap.add_argument("--batch-size", type=int, default=64)
    ap.add_argument("--concurrency", type=int, default=API_CONCURRENCY,
                    help="동시 요청 수. 왕복 지연이 지배적이라 그대로 처리량이 된다")
    ap.add_argument("--threads", type=int, default=None, help="local 백엔드 전용")
    ap.add_argument("--limit", type=int, default=None,
                    help="앞 N개 인스턴스만. 스모크 테스트용")
    ap.add_argument("--dry-run", action="store_true",
                    help="인코딩 없이 청크·문자 수만 센다. 비용 확인용")
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

    enc = build_encoder(a)
    res = run(repo_dirs, candidates, conditions=conds, enc=enc,
              cache_path=a.cache or (a.out_dir / "embed_blobs.npz"),
              autosave_every=a.autosave_every, limit=a.limit,
              dry_run=a.dry_run, out_dir=a.out_dir)

    print(f"\nfingerprint: {res['fingerprint']}")
    c = res["corpus"]
    print(f"corpus: blobs={c['n_blobs']:,} cached={c['n_cached']:,} "
          f"encoded={c['n_need']:,} chunks={c['n_chunks']:,} chars={c['n_chars']:,}")
    if res.get("dry_run"):
        # 실제 토크나이저가 센 값이다 (휴리스틱 아님). 겹침 구간은 두 번
        # 실려 나가므로 두 번 세는 게 맞다 — API는 보낸 만큼 과금한다.
        tok = enc.stats.n_tokens
        print(f"DRY RUN — 실측 토큰 {tok:,}  "
              f"({a.model} $0.01/1M 기준 ≈ ${tok / 1e6 * 0.01:.2f})")
        return
    print(f"instances={res['n_instances']} ok={res['ok']} failed={len(res['failed'])}")
    for iid, why in res["failed"][:20]:
        print(f"  FAIL {iid}: {why}")
    print(f"encode: {res['stats']}")
    print(f"rows={res['n_rows']}  ->  {res['out_dir']}")


if __name__ == "__main__":
    main()
