"""코드 인지 토크나이저.

일반 텍스트 토크나이저를 코드에 그대로 쓰면 `get_records`가 통째로 하나의 토큰이
되어 `getRecords`나 `records`와 절대 매칭되지 않는다. 반대로 무조건 분해만 하면
`get`, `set` 같은 무의미한 토큰이 문서를 지배한다.

그래서 **원본 식별자와 subtoken을 둘 다 남긴다.** 정확 매칭은 원본 토큰이 받고,
표기 차이(snake/camel)는 subtoken이 흡수한다. BM25의 IDF가 알아서
`get`(흔함, 낮은 가중치)과 `caplog`(희귀, 높은 가중치)를 구분해 준다.
"""

from __future__ import annotations

import re

_WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_]*|\d+")
_CAMEL = re.compile(r"[A-Z]+(?![a-z])|[A-Z][a-z0-9]*|[a-z0-9]+")

# 영어 불용어 + 이슈 리포트/코드에서 의미 없이 흔한 토큰.
STOPWORDS = frozenset("""
a an the and or but if then else of to in on at by for with without from as is are was were
be been being do does did done have has had having will would shall should can could may might
must not no nor so such than that this these those it its it's i we you he she they them us
what which who whom whose when where why how all any both each few more most other some only
own same too very just also into out up down over under again further once here there
i'm i've don't doesn't isn't aren't wasn't weren't didn't won't can't cannot
self cls none true false def class return import lambda pass raise yield with try except
finally elif while for print len str int float bool list dict set tuple type object super
py python version issue bug error problem expected actual example code line lines file files
please thanks hi hello reproduce reproducible steps environment details description
""".split())

MIN_LEN = 2


def tokenize(text: str, *, keep_stopwords: bool = False) -> list[str]:
    """텍스트를 BM25 토큰 리스트로 변환한다.

    - 소문자화
    - `snake_case` / `camelCase` / `CONST_CASE`를 subtoken으로 분해
    - 원본 식별자도 함께 유지 (분해형과 중복 계상되는 것은 의도된 동작이다)
    - 길이 1 토큰과 불용어 제거
    """
    tokens: list[str] = []
    for m in _WORD.finditer(text):
        raw = m.group(0)
        low = raw.lower().strip("_")
        if not low:
            continue

        parts = [p.lower() for p in _CAMEL.findall(raw) if p]
        # 분해 결과가 원본과 같으면(단일 단어) 한 번만 넣는다.
        if len(parts) > 1:
            if len(low) >= MIN_LEN and (keep_stopwords or low not in STOPWORDS):
                tokens.append(low)
            for p in parts:
                if len(p) >= MIN_LEN and (keep_stopwords or p not in STOPWORDS):
                    tokens.append(p)
        else:
            if len(low) >= MIN_LEN and (keep_stopwords or low not in STOPWORDS):
                tokens.append(low)
    return tokens


def tokenize_path(file_id: str) -> list[str]:
    """파일 경로를 토큰화한다.

    `src/_pytest/logging.py` → src, pytest, logging, py, _pytest/logging.py 의 조각.
    확장자는 모든 문서에 공통이라 IDF가 0에 수렴하므로 굳이 빼지 않는다.
    """
    return tokenize(file_id.replace("/", " ").replace(".", " ").replace("-", " "))
