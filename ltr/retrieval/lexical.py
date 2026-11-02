"""Code lexical features shared with training; no dataset or Git dependency."""

from __future__ import annotations

import math
import re
from collections import Counter, defaultdict
from typing import Iterable, Sequence

from .tokenizer import STOPWORDS

MIN_IDENT_LEN = 3
MIN_SEGMENT_CHARS = 12
MIN_SEGMENT_WORDS = 2
MIN_COVERAGE = 0.99

_DEF = re.compile(r"^[ \t]*(?:async[ \t]+)?(?:def|class)[ \t]+([A-Za-z_]\w*)", re.M)
_IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_TRIPLE = re.compile(r'(?s)("""|\'\'\').*?\1')
_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"|\'((?:[^\'\\\n]|\\.)*)\'')
# str.format / f-string 치환부와 %-포맷 지정자. 리터럴을 이 자리에서 끊어 고정 조각만 남긴다.
_PLACEHOLDER = re.compile(
    r"\{[^{}]*\}"
    r"|%\([^)]*\)[#0 +-]*\d*(?:\.\d+)?[a-zA-Z]"
    r"|%[#0 +-]*\d*(?:\.\d+)?[sdrifgeExXco]")
_ALPHA_WORD = re.compile(r"[a-z]{2,}")
_WS = re.compile(r"\s+")
# 들여쓰기 없는 대입 `NAME = ...` / `NAME: T = ...`. `==` 비교는 제외.
_MODNAME = re.compile(r"^([A-Za-z_]\w*)[ \t]*(?::[^=\n]*)?=(?!=)", re.M)
_COMMENT = re.compile(r"#[^\n]*")


def normalize_text(text: str) -> str:
    return _WS.sub(" ", text.lower()).strip()


def defined_names(source: str) -> frozenset[str]:
    """파일이 정의하는 함수·클래스 이름 (소문자). 매직 메서드는 뺀다."""
    return frozenset(
        n.lower() for n in _DEF.findall(source)
        if len(n) >= MIN_IDENT_LEN and not (n.startswith("__") and n.endswith("__")))


def module_names(source: str) -> frozenset[str]:
    """모듈 수준 대입 이름 (소문자). `__all__` 같은 dunder는 뺀다."""
    return frozenset(
        n.lower() for n in _MODNAME.findall(source)
        if len(n) >= MIN_IDENT_LEN and not (n.startswith("__") and n.endswith("__")))


def used_identifiers(source: str) -> frozenset[str]:
    """코드에 등장하는 식별자 (소문자). docstring·주석은 지운다 — 설명문 속 단어는 사용이 아니다.

    정의 이름도 포함된다. "사용만 하는" 판정은 호출부가 정의 집합을 빼서 한다.
    """
    body = _COMMENT.sub(" ", _TRIPLE.sub(" ", source))
    return frozenset(w.lower() for w in _IDENT.findall(body) if len(w) >= MIN_IDENT_LEN)


def query_identifiers(query: str) -> frozenset[str]:
    """쿼리의 식별자 후보 (소문자). 불용어는 `tokenizer.STOPWORDS`와 같은 목록."""
    return frozenset(
        w.lower() for w in _IDENT.findall(query)
        if len(w) >= MIN_IDENT_LEN and w.lower() not in STOPWORDS)


def literal_segments(source: str) -> frozenset[str]:
    """한 줄짜리 문자열 리터럴의 고정 조각들 (정규화됨).

    docstring(삼중 따옴표)은 먼저 지운다 — 설명문이 이슈 산문과 겹치는 것은
    "이 파일이 그 메시지를 낸다"는 신호가 아니다.
    """
    body = _TRIPLE.sub(" ", source)
    out = set()
    for m in _LITERAL.finditer(body):
        lit = m.group(1) if m.group(1) is not None else m.group(2)
        lit = lit.replace("\\n", " ").replace("\\t", " ")
        for piece in _PLACEHOLDER.split(lit):
            seg = normalize_text(piece)
            if (len(seg) >= MIN_SEGMENT_CHARS
                    and len(_ALPHA_WORD.findall(seg)) >= MIN_SEGMENT_WORDS):
                out.add(seg)
    return frozenset(out)


def _idf(df: int, n: int) -> float:
    """`seed.bm25.BM25.idf`와 같은 평활화형."""
    return math.log(1.0 + (n - df + 0.5) / (df + 0.5))


def _idf_sum_scores(per_file: Sequence[frozenset[str]], hits: set[str]) -> list[float]:
    # 합산 순서를 정렬로 고정한다. set 순회 순서는 문자열 해시 무작위화로 실행마다
    # 달라지고, 부동소수점 덧셈 순서가 바뀌면 1e-14 수준 차이가 동점 순서를 뒤집는다.
    df = Counter(x for items in per_file for x in items if x in hits)
    n = len(per_file)
    return [sum(_idf(df[x], n) for x in sorted(items) if x in hits) for items in per_file]


def def_match_scores(defs: Sequence[frozenset[str]], query_ids: frozenset[str]) -> list[float]:
    """파일별 Σ idf(이름) — 쿼리 식별자 중 그 파일이 정의하는 것만."""
    return _idf_sum_scores(defs, set(query_ids))


def str_match_scores(segs: Sequence[frozenset[str]], query_norm: str) -> list[float]:
    """파일별 Σ idf(조각) — 정규화된 쿼리에 부분문자열로 등장하는 조각만."""
    universe = {s for items in segs for s in items}
    hits = {s for s in universe if s in query_norm}
    return _idf_sum_scores(segs, hits)


def symbol_scores(defs: Sequence[frozenset[str]], used: Sequence[frozenset[str]],
                  file_ids: Sequence[str], query_ids: frozenset[str],
                  edges: Iterable[tuple[str, str]]) -> tuple[list[float], list[float]]:
    """(sym_use, sym_prop) — 파일 순서는 `file_ids`와 같다.

    `defs[i]`는 i번 파일의 정의 이름(함수·클래스·모듈 대입). 이슈 식별자 중 정의가
    **하나라도 있는** 것만 심볼로 본다 — 정의 없는 단어는 저장소의 개념이 아니다.
    idf는 **사용 df**로 센다. 수백 파일이 부르는 심볼은 가리키는 힘이 약하다.
    """
    definers: dict[str, set[str]] = defaultdict(set)
    for fid, d in zip(file_ids, defs):
        for s in d & query_ids:
            definers[s].add(fid)
    symbols = set(definers)

    only_used = [(u & symbols) - d for u, d in zip(used, defs)]
    df = Counter(s for items in only_used for s in items)
    n = len(file_ids)

    nbrs: dict[str, set[str]] = defaultdict(set)
    for s, t in edges:
        nbrs[s].add(t)
        nbrs[t].add(s)

    use, prop = [], []
    for fid, items in zip(file_ids, only_used):
        ordered = sorted(items)       # 합산 순서 고정 (`_idf_sum_scores` 주석)
        use.append(sum(_idf(df[s], n) for s in ordered))
        near = nbrs.get(fid, set())
        prop.append(sum(_idf(df[s], n) for s in ordered if definers[s] & near))
    return use, prop


def neighbor_aggregates(file_ids: Iterable[str], bm25: dict[str, float],
                        edges: Iterable[tuple[str, str]]) -> tuple[dict, dict]:
    """무방향 이웃의 BM25 (최댓값, 평균). 이웃이 없으면 0.0 — 결측이 아니라 실제 0이다."""
    nbrs: dict[str, set[str]] = defaultdict(set)
    for s, t in edges:
        nbrs[s].add(t)
        nbrs[t].add(s)
    mx, mean = {}, {}
    for f in file_ids:
        vals = [bm25[n] for n in sorted(nbrs.get(f, ())) if n in bm25]
        mx[f] = max(vals) if vals else 0.0
        mean[f] = sum(vals) / len(vals) if vals else 0.0
    return mx, mean


