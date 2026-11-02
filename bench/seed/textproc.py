"""Query preprocessing compatibility imports and benchmark leakage diagnostics."""

from ltr.retrieval.textproc import (
    CONDITIONS, FULL, NO_PATHS, NO_TRACE, make_query, strip_paths, strip_tracebacks,
)

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
