"""인스턴스별 문서 corpus 구성.

BM25 문서 = `base_commit` 시점의 소스 파일 하나.
문서 텍스트 = 파일 경로 토큰 + 파일 내용 토큰.

**base_commit 시점 코드만 쓴다** (CONTRACT.md §3-3). 패치 적용 후 트리를 읽으면
gold 파일에 정답 코드가 들어 있어 실험 전체가 무효가 된다. 그래서 워킹트리를
전혀 건드리지 않고 `git ls-tree` + `git cat-file --batch` 로만 읽는다.
체크아웃을 하지 않으니 `bench-harness`가 같은 클론을 동시에 써도 서로 깨지지 않는다.

blob SHA로 토큰 캐시를 둔다. SWE-bench의 한 repo 인스턴스들은 커밋이 서로 가까워
파일 대부분이 인스턴스 간 동일하다. 캐시가 없으면 같은 파일을 100번 토큰화한다.
"""

from __future__ import annotations

import subprocess
from dataclasses import dataclass
from pathlib import Path

from bench.schema import normalize_file_id
from bench.seed.tokenizer import tokenize, tokenize_path

# 인덱싱 대상 확장자. Telescode 파서가 Python만 다루므로 .py로 맞춘다.
SOURCE_SUFFIXES = (".py",)

# 파일당 인덱싱 상한(바이트). 거대한 생성 파일 하나가 avgdl을 왜곡하는 것을 막는다.
MAX_BYTES = 400_000

# blob sha -> 토큰 리스트
_TOKEN_CACHE: dict[str, list[str]] = {}


@dataclass
class Corpus:
    instance_id: str
    doc_ids: list[str]              # 정규화된 file_id
    docs: list[list[str]]           # 토큰 리스트
    skipped: list[tuple[str, str]]  # (file_id, 사유)


def list_source_blobs(repo_dir: Path, commit: str) -> list[tuple[str, str]]:
    """`commit` 시점 트리의 소스 파일 `(file_id, blob_sha)` 목록."""
    r = subprocess.run(["git", "-C", str(repo_dir), "ls-tree", "-r", commit],
                       capture_output=True, check=True)
    out = []
    for line in r.stdout.decode("utf-8", errors="replace").splitlines():
        # "<mode> <type> <sha>\t<path>"
        meta, _, path = line.partition("\t")
        parts = meta.split()
        if len(parts) != 3 or parts[1] != "blob":
            continue
        if not path.endswith(SOURCE_SUFFIXES):
            continue
        out.append((normalize_file_id(path), parts[2]))
    return out


def _batch_read(repo_dir: Path, shas: list[str]) -> dict[str, bytes]:
    """`git cat-file --batch`로 blob 내용을 한 번에 읽는다.

    파일당 프로세스를 띄우면 119 인스턴스 × ~800 파일 = 9.5만 번 fork가 된다.
    """
    if not shas:
        return {}
    p = subprocess.Popen(["git", "-C", str(repo_dir), "cat-file", "--batch"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    stdout, _ = p.communicate(("\n".join(shas) + "\n").encode())

    out: dict[str, bytes] = {}
    pos = 0
    n = len(stdout)
    while pos < n:
        nl = stdout.find(b"\n", pos)
        if nl < 0:
            break
        header = stdout[pos:nl].decode("utf-8", errors="replace").split()
        pos = nl + 1
        if len(header) < 3:      # "<sha> missing"
            continue
        sha, _typ, size = header[0], header[1], int(header[2])
        out[sha] = stdout[pos:pos + size]
        pos += size + 1          # 내용 뒤 개행
    return out


def build_corpus(repo_dir: Path, instance_id: str, commit: str,
                 *, path_weight: int = 1,
                 candidate_file_ids: set[str] | None = None) -> Corpus:
    """인스턴스 하나의 corpus를 만든다.

    Args:
        candidate_file_ids: 주어지면 이 집합으로 문서를 제한한다.
            `bench-harness`의 스캔 결과(= 피처 매트릭스의 file_id)를 넘기면
            corpus가 매트릭스와 정확히 정렬돼 조인 손실이 0이 된다.
            None이면 트리의 모든 `.py`를 쓴다 (커버리지는 조인 시 보고).
        path_weight: 경로 토큰을 몇 번 반복해 넣을지. 경로는 강한 신호인 동시에
            누출 통로이기도 하다. 기본 1 = 한 번만.
    """
    blobs = list_source_blobs(repo_dir, commit)
    if candidate_file_ids is not None:
        blobs = [(p, s) for p, s in blobs if p in candidate_file_ids]

    need = sorted({s for _, s in blobs if s not in _TOKEN_CACHE})
    contents = _batch_read(repo_dir, need)

    doc_ids: list[str] = []
    docs: list[list[str]] = []
    skipped: list[tuple[str, str]] = []

    for path, sha in blobs:
        toks = _TOKEN_CACHE.get(sha)
        if toks is None:
            raw = contents.get(sha)
            if raw is None:
                skipped.append((path, "blob-missing"))
                continue
            if len(raw) > MAX_BYTES:
                raw = raw[:MAX_BYTES]
            toks = tokenize(raw.decode("utf-8", errors="replace"))
            _TOKEN_CACHE[sha] = toks

        full = tokenize_path(path) * max(0, path_weight) + toks
        if not full:
            skipped.append((path, "empty-after-tokenize"))
            continue
        doc_ids.append(path)
        docs.append(full)

    return Corpus(instance_id=instance_id, doc_ids=doc_ids, docs=docs, skipped=skipped)
