"""어휘 중첩(vocabulary overlap) 구간 정의.

이 실험의 핵심 주장은 "임베딩·어휘 검색은 못 찾고 그래프는 찾는다"이다.
그 주장이 성립하는 구간이 **어휘가 겹치지 않는 gold 파일**이다. `auth/login.py`가
상속하는 세션 클래스에 "login"이 한 번도 안 나오면, BM25는 원리상 그 파일을 찾을 수
없다. 전체 평균에서 BM25에 져도 이 구간에서 그래프가 이기면, 그게 Telescode가
검색 도구가 아니라는 증거다.

그래서 이 구간의 **정의 자체가 최종 결론의 근거**가 된다. 아래를 명시한다.

## 판정 기준

파일 `f`가 인스턴스 `i`에서 **overlap** 이려면:

    | Q(i) ∩ D(f) ∩ Distinctive(i) |  >=  MIN_OVERLAP_TOKENS

  - `Q(i)`  : 이슈 텍스트의 토큰 집합. 조건(full/no_trace/no_paths)별로 다르다.
  - `D(f)`  : 파일의 **식별자** 토큰 집합 — 경로 토큰 + 클래스명 + 함수명.
  - `Distinctive(i)` : 그 인스턴스 corpus에서 식별자 df 비율이 `MAX_DF_RATIO`
                       이하인 토큰. `test`, `get`, `data`처럼 거의 모든 파일에
                       등장하는 토큰은 겹쳐도 검색 신호가 아니다.

토큰화는 **BM25 인덱싱과 완전히 같은 함수**(`tokenizer.tokenize` /
`tokenize_path`)를 쓴다. 다르면 "겹침" 판정이 BM25 실력과 어긋난다.

## 왜 파일 내용 전체가 아니라 식별자인가

BM25는 파일 **본문 전체**를 인덱싱한다. 그런데 본문 기준으로 중첩을 판정하면
`return`, `array`, `value` 같은 흔한 단어 때문에 사실상 모든 파일이 overlap이 되어
구간이 비어 버린다. 우리가 알고 싶은 것은 "이슈가 이 파일의 **개념을 지목했는가**"
이므로 식별자로 좁힌다. 이건 BM25에 **불리한** 정의가 아니라 중립적이다 —
overlap 구간에도 BM25가 못 찾는 파일이 얼마든지 들어갈 수 있다.

## 임계값

`MIN_OVERLAP_TOKENS = 1`, `MAX_DF_RATIO = 0.10` 이 기본값이다. 둘 다 자의적이므로
`sweep_definitions()`로 임계값을 흔들었을 때 결론이 바뀌는지 함께 보고한다.
결론이 임계값에 의존하면 그 결론은 쓸 수 없다.

## 클래스·함수명 출처

Telescode DB가 아니라 Python `ast`로 `base_commit` 시점 원문에서 직접 뽑는다.
인스턴스 DB는 피처 추출 직후 삭제되고(CONTRACT.md §3-6), 파서 수정은 내 담당이
아니기 때문이다. 파서와 이름이 미세하게 다를 수 있으나 file_id 키는 동일하므로
재스캔 후 그래프 결과와 그대로 조인된다.
"""

from __future__ import annotations

import ast
from dataclasses import dataclass, field
from pathlib import Path

from bench.schema import normalize_file_id
from bench.seed.corpus import _batch_read, list_source_blobs
from bench.seed.tokenizer import tokenize, tokenize_path

MIN_OVERLAP_TOKENS = 1
MAX_DF_RATIO = 0.10

# 산출 CSV의 컬럼명. `bench/schema.py`에는 추가하지 않는다 —
# 이건 학습 피처가 아니라 **평가 구간 라벨**이고, 피처로 쓰면 쿼리 의존 라벨이
# 학습에 새어 들어간다.
COL_OVERLAP = "vocab_overlap"
COL_N_OVERLAP = "n_overlap_tokens"
COL_OVERLAP_TOKENS = "overlap_tokens"


@dataclass
class IdentifierIndex:
    """인스턴스 하나의 파일별 식별자 토큰 집합."""

    instance_id: str
    ids: dict[str, set[str]] = field(default_factory=dict)   # file_id -> 토큰
    parse_failed: list[str] = field(default_factory=list)

    @property
    def n_files(self) -> int:
        return len(self.ids)

    def distinctive(self, max_df_ratio: float = MAX_DF_RATIO) -> set[str]:
        """df 비율이 임계 이하인 토큰만. 흔한 토큰은 겹쳐도 신호가 아니다."""
        if not self.ids:
            return set()
        df: dict[str, int] = {}
        for toks in self.ids.values():
            for t in toks:
                df[t] = df.get(t, 0) + 1
        # 하한 1: 단 하나의 파일에만 등장하는 토큰은 정의상 가장 변별력이 높다.
        # `max()`가 없으면 파일 수가 적을 때(1/N > 비율) 모든 토큰이 걸러져
        # distinctive가 공집합이 되고, 그러면 전부 non_overlap으로 오분류된다.
        limit = max(1.0, max_df_ratio * self.n_files)
        return {t for t, c in df.items() if c <= limit}


def file_identifier_tokens(source: str, file_id: str,
                           *, include_module_names: bool = False) -> tuple[set[str], bool]:
    """파일의 식별자 토큰 집합과 파싱 성공 여부.

    경로 토큰 + 클래스명 + 함수명(중첩 포함). `include_module_names`면 모듈 수준
    대입 이름(`DEFAULT_ENCODING` 등)도 넣는다 — 민감도 확인용 변형이다.
    """
    toks = set(tokenize_path(file_id))
    try:
        tree = ast.parse(source)
    except (SyntaxError, ValueError):
        return toks, False

    for node in ast.walk(tree):
        if isinstance(node, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
            toks.update(tokenize(node.name))
    if include_module_names:
        for node in tree.body:
            targets = []
            if isinstance(node, ast.Assign):
                targets = node.targets
            elif isinstance(node, ast.AnnAssign):
                targets = [node.target]
            for t in targets:
                if isinstance(t, ast.Name):
                    toks.update(tokenize(t.id))
    return toks, True


def build_identifier_index(repo_dir: Path, instance_id: str, commit: str,
                           *, candidate_file_ids: set[str] | None = None,
                           include_module_names: bool = False) -> IdentifierIndex:
    blobs = list_source_blobs(repo_dir, commit)
    if candidate_file_ids is not None:
        blobs = [(p, s) for p, s in blobs if p in candidate_file_ids]

    contents = _batch_read(repo_dir, sorted({s for _, s in blobs}))
    idx = IdentifierIndex(instance_id=instance_id)
    for path, sha in blobs:
        raw = contents.get(sha)
        if raw is None:
            idx.parse_failed.append(path)
            continue
        toks, ok = file_identifier_tokens(
            raw.decode("utf-8", errors="replace"), path,
            include_module_names=include_module_names)
        idx.ids[path] = toks
        if not ok:
            idx.parse_failed.append(path)
    return idx


def overlap_for_instance(idx: IdentifierIndex, query_tokens: set[str],
                         *, min_tokens: int = MIN_OVERLAP_TOKENS,
                         max_df_ratio: float = MAX_DF_RATIO
                         ) -> dict[str, tuple[bool, int, list[str]]]:
    """file_id -> (overlap 여부, 겹친 distinctive 토큰 수, 토큰 목록)."""
    distinct = idx.distinctive(max_df_ratio)
    out = {}
    for fid, toks in idx.ids.items():
        hit = sorted(toks & query_tokens & distinct)
        out[normalize_file_id(fid)] = (len(hit) >= min_tokens, len(hit), hit)
    return out
