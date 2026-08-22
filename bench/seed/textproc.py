"""쿼리 텍스트 전처리 — 스택 트레이스 / 파일경로 누출 제거.

**왜 이 모듈이 필요한가.**
SWE-bench `problem_statement`에는 gold 파일 경로가 그대로 박혀 있는 경우가 흔하다.
그러면 BM25가 "검색을 잘해서"가 아니라 "정답을 베껴서" 높은 점수를 낸다.
누출 경로는 스택 트레이스만이 아니다. 실제 pytest 인스턴스를 보면 최소 네 가지다.

  1. Python traceback:  ``File "/x/src/_pytest/logging.py", line 699, in ...``
  2. pytest 출력 위치:   ``src/_pytest/logging.py:345: in caplog_clear`` /
                        ``SKIPPED [1] conftest.py:6: Skipping``
  3. GitHub 퍼머링크:    ``https://github.com/.../blob/<sha>/src/_pytest/logging.py#L699``
  4. 산문 내 직접 언급:  "the fix belongs in ``src/_pytest/logging.py``"

그래서 조건을 계층으로 정의한다. 팀 계약이 요구하는 것은 FULL / NO_TRACE 두 개이고,
NO_PATHS는 "경로 누출이 하나도 없을 때의 진짜 실력"을 보기 위한 상한 통제다.
"""

from __future__ import annotations

import re

# ── 조건 이름 ─────────────────────────────────────────────────────────────
FULL = "full"          # 원문 그대로
NO_TRACE = "no_trace"  # 트레이스백/테스트출력 위치 라인 제거 (계약 필수 조건)
NO_PATHS = "no_paths"  # NO_TRACE + 소스경로처럼 보이는 토큰 전부 마스킹

CONDITIONS = (FULL, NO_TRACE, NO_PATHS)

# ── 정규식 ────────────────────────────────────────────────────────────────

# 1. traceback 헤더 ~ 마지막 프레임. 예외 요약 줄(마지막)은 남긴다.
#    예외 타입/메시지는 사람이 쓴 증상 설명에 가까워 누출로 보기 어렵다.
_TB_HEADER = re.compile(r"^\s*Traceback \(most recent call last\):\s*$", re.M)

# 2. ``File "....py", line 12, in foo`` — 트레이스백 프레임 줄
_TB_FRAME = re.compile(r'^\s*File "[^"]*", line \d+(?:, in .*)?$', re.M)

# 3. pytest / doctest 스타일 위치 줄
#    ``src/_pytest/x.py:345: in fn`` , ``src/_pytest/x.py:345: AssertionError``
#    ``SKIPPED [1] conftest.py:6: reason`` , ``E   src/x.py:3: Error``
_PYTEST_LOC = re.compile(
    r"^(?:E\s+|_+\s*)?"
    r"(?:(?:SKIPPED|XFAIL|XPASS|FAILED|ERROR|PASSED)\s*(?:\[\d+\])?\s*)?"
    r"[\w./\\-]+\.(?:py|pyi|pyx|txt|rst|cfg|ini|toml)"
    r":\d+(?::\d*)?:.*$",
    re.M,
)

# 4. ``at /path/to/file.py:12`` 같은 인라인 위치
_INLINE_LOC = re.compile(r"[\w./\\-]+\.(?:py|pyi|pyx)\s*:\s*\d+")

# 5. 경로처럼 보이는 토큰 전부 (NO_PATHS 전용).
#    ``src/_pytest/logging.py`` , ``_pytest/logging.py`` , ``logging.py``
#    URL 안에 들어 있어도 잡힌다.
_PATHY = re.compile(r"[\w][\w./\\-]*\.(?:py|pyi|pyx|c|h|cpp|hpp|js|ts|java|go|rs)\b")

# 6. GitHub blob/tree 퍼머링크 — 경로 전체가 URL에 들어 있다
_GH_PERMALINK = re.compile(
    r"https?://(?:www\.)?github\.com/[\w.-]+/[\w.-]+/(?:blob|tree|raw)/\S+"
)

_MASK = " "


def strip_tracebacks(text: str) -> str:
    """Traceback 블록과 테스트 출력 위치 라인을 제거한다.

    프레임 줄과 그 바로 다음의 소스 코드 줄(들여쓰기된 한 줄)을 함께 지운다.
    소스 줄을 남기면 gold 파일의 실제 코드가 쿼리에 그대로 들어가 버린다.
    """
    lines = text.split("\n")
    out: list[str] = []
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        if _TB_HEADER.match(line):
            i += 1
            continue
        if _TB_FRAME.match(line):
            i += 1
            # 프레임 뒤에 붙는 들여쓰기된 소스 인용 줄들을 같이 버린다.
            while i < n and lines[i].startswith(("    ", "\t")) and lines[i].strip():
                i += 1
            continue
        if _PYTEST_LOC.match(line):
            i += 1
            continue
        out.append(line)
        i += 1

    joined = "\n".join(out)
    joined = _GH_PERMALINK.sub(_MASK, joined)
    joined = _INLINE_LOC.sub(_MASK, joined)
    return joined


def strip_paths(text: str) -> str:
    """소스 파일 경로처럼 보이는 토큰을 전부 마스킹한다 (NO_PATHS 전용)."""
    return _PATHY.sub(_MASK, text)


def make_query(problem_statement: str, condition: str) -> str:
    """조건별 쿼리 텍스트를 만든다."""
    if condition == FULL:
        return problem_statement
    if condition == NO_TRACE:
        return strip_tracebacks(problem_statement)
    if condition == NO_PATHS:
        return strip_paths(strip_tracebacks(problem_statement))
    raise ValueError(f"unknown condition: {condition!r} (expected one of {CONDITIONS})")


# ── 누출 진단 ─────────────────────────────────────────────────────────────

def mentions_path(text: str, file_id: str) -> bool:
    """`file_id`가 텍스트에 사실상 그대로 등장하는지.

    전체 경로(`src/_pytest/logging.py`)와 basename(`logging.py`) 둘 다 본다.
    basename만 맞아도 BM25에는 강한 신호라서 누출로 센다.
    """
    if not text or not file_id:
        return False
    lowered = text.lower()
    fid = file_id.lower()
    if fid in lowered:
        return True
    base = fid.rsplit("/", 1)[-1]
    # ``logging.py`` 처럼 흔한 basename은 오탐이 있지만, 누출을 과소평가하는 것보다
    # 과대평가하는 쪽이 안전하다. 두 수치를 따로 보고한다.
    return bool(base) and base in lowered


def leakage_report(problem_statement: str, gold_file_ids: set[str]) -> dict:
    """조건별로 gold 경로가 쿼리에 남아 있는지 센다."""
    out = {}
    for cond in CONDITIONS:
        q = make_query(problem_statement, cond).lower()
        full_hits = sum(1 for g in gold_file_ids if g.lower() in q)
        base_hits = sum(1 for g in gold_file_ids if mentions_path(q, g))
        out[cond] = {
            "gold_n": len(gold_file_ids),
            "full_path_hits": full_hits,
            "basename_hits": base_hits,
            "any_full_path": full_hits > 0,
            "any_basename": base_hits > 0,
        }
    return out
