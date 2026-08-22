"""SWE-bench 인스턴스 로더 (읽기 전용).

수집(`bench/collect/`)은 `bench-harness` 소유다. 이 모듈은 그 결과를 기다리지 않고
**쿼리와 gold 라벨만** 로컬 HF 캐시의 parquet에서 직접 읽는다.
쿼리 전처리는 `retrieval`의 영역이므로 소유권 충돌이 아니다.

`bench-harness`가 인스턴스 목록을 확정하면 `--instances` 로 그 목록을 넘겨
같은 집합으로 좁힌다.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass
from pathlib import Path

import pandas as pd

from bench.schema import normalize_file_id

# HF 캐시의 SWE-bench test split. 환경변수로 덮어쓸 수 있다.
_DEFAULT_PARQUET = (
    Path.home()
    / ".cache/huggingface/hub/datasets--princeton-nlp--SWE-bench"
    / "snapshots/e48e2bd1e9fecd5bbd641e9414ac59da9f2e69f6/data/test-00000-of-00001.parquet"
)

_DIFF_GIT = re.compile(r"^diff --git a/(?P<a>\S+) b/(?P<b>\S+)$", re.M)


@dataclass(frozen=True)
class Instance:
    instance_id: str
    repo: str
    base_commit: str
    problem_statement: str
    gold_files: frozenset[str]


def parquet_path() -> Path:
    p = os.environ.get("SWEBENCH_PARQUET")
    if p:
        return Path(p)
    if _DEFAULT_PARQUET.exists():
        return _DEFAULT_PARQUET
    # 스냅샷 해시가 바뀌었을 수 있으니 캐시 디렉토리를 훑는다.
    root = Path.home() / ".cache/huggingface/hub/datasets--princeton-nlp--SWE-bench"
    hits = sorted(root.glob("snapshots/*/data/*.parquet"))
    if hits:
        return hits[0]
    raise FileNotFoundError(
        "SWE-bench parquet을 찾을 수 없다. SWEBENCH_PARQUET 환경변수로 지정하라."
    )


def gold_files_from_patch(patch: str) -> frozenset[str]:
    """gold patch가 건드린 파일 경로 집합.

    `diff --git a/X b/Y`에서 b/ 쪽을 쓰되, 삭제(`b/dev/null`)면 a/ 쪽을 쓴다.
    `file_id` 정규화는 반드시 `bench.schema.normalize_file_id`로 한다 —
    하네스와 다른 규칙을 쓰면 조인이 조용히 0행이 된다 (CONTRACT.md §1).
    """
    out = set()
    for m in _DIFF_GIT.finditer(patch or ""):
        a, b = m.group("a"), m.group("b")
        path = a if b == "dev/null" else b
        out.add(normalize_file_id(path))
    return frozenset(out)


def load_instances(repo: str | None = None,
                   instance_ids: list[str] | None = None,
                   limit: int | None = None) -> list[Instance]:
    df = pd.read_parquet(parquet_path())
    if repo:
        df = df[df["repo"] == repo]
    if instance_ids:
        df = df[df["instance_id"].isin(instance_ids)]
    df = df.sort_values("instance_id")
    if limit:
        df = df.head(limit)

    out = []
    for r in df.itertuples(index=False):
        out.append(Instance(
            instance_id=r.instance_id,
            repo=r.repo,
            base_commit=r.base_commit,
            problem_statement=r.problem_statement or "",
            gold_files=gold_files_from_patch(r.patch),
        ))
    return out
