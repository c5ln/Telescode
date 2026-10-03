"""평가 배관 테스트 — 지표 자체가 아니라 **연결**을 검증한다.

지표 정확성은 `bench/metrics/` 소유이고 그쪽 테스트가 담당한다.
"""
import pandas as pd
import pytest

from bench.schema import C
from bench.seed.evaluate import evaluate_condition, metrics_available


def _df(rows):
    return pd.DataFrame(rows, columns=[C.instance_id, C.file_id, C.bm25])


def _scanned(df, extra=()):
    """기본은 후보 = 스캔 전체. `extra`로 스캔에만 있는 파일을 추가한다."""
    out = {k: set(v) for k, v in df.groupby(C.instance_id)[C.file_id].apply(set).items()}
    for iid, f in extra:
        out.setdefault(iid, set()).add(f)
    return out


def test_metrics_are_imported_not_reimplemented():
    import inspect

    import bench.metrics.ranking as R
    import bench.seed.evaluate as E
    for name in ("recall_at_k", "mrr", "ndcg_at_k", "reachable_recall_ceiling",
                 "apply_pessimistic_tiebreak", "evaluate_instance"):
        fn = getattr(E, name, None)
        if fn is not None:
            assert inspect.getmodule(fn) is R, f"{name}이 seed에서 재구현됐다"


def test_perfect_ranking():
    assert metrics_available()
    d = _df([("i1", "a.py", 9.0), ("i1", "b.py", 1.0)])
    r = evaluate_condition(d, {"i1": {"a.py"}}, scanned=_scanned(d),
                           condition="full", ks=(1, 2))
    s = r.summary
    assert s["recall@1"] == 1.0 and s["mrr"] == 1.0 and s["reachable_recall_ceiling"] == 1.0


def test_tiebreak_is_pessimistic_end_to_end():
    # 동점이면 gold가 뒤로 가야 하므로 recall@1 == 0
    d = _df([("i1", "a.py", 5.0), ("i1", "b.py", 5.0)])
    r = evaluate_condition(d, {"i1": {"a.py"}}, scanned=_scanned(d),
                           condition="full", ks=(1, 2))
    assert r.summary["recall@1"] == 0.0
    assert r.summary["recall@2"] == 1.0


def test_gold_missing_from_candidates_lowers_ceiling():
    d = _df([("i1", "a.py", 1.0)])
    r = evaluate_condition(d, {"i1": {"a.py", "ghost.py"}}, scanned=_scanned(d),
                           condition="full", ks=(1,))
    assert r.summary["reachable_recall_ceiling"] == 0.5
    assert r.summary["recall@1"] == 0.5, "분모는 gold 전체다 (스캔에 없는 것 포함)"


def test_truncated_candidates_do_not_inflate_ceiling():
    """후보를 자르면 Recall은 떨어지되 ceiling은 스캔 기준으로 남아야 한다.

    ceiling을 후보집합에서 계산하던 옛 방식이면 여기서 1.0이 나와, 낮은 Recall이
    '알고리즘 탓'으로 보인다. 실제로는 후보 절단 탓이다.
    """
    cand = _df([("i1", "a.py", 9.0)])                 # b.py를 top-N에서 잘라냄
    scanned = {"i1": {"a.py", "b.py"}}                # 스캐너는 둘 다 봤다
    r = evaluate_condition(cand, {"i1": {"a.py", "b.py"}}, scanned=scanned,
                           condition="full", ks=(1, 2))
    assert r.summary["reachable_recall_ceiling"] == 1.0, "스캔에는 gold가 다 있다"
    assert r.summary["recall@2"] == 0.5, "후보에서 잘린 gold는 못 맞힌다"
    assert r.summary["n_scanned"] == 2 and r.summary["n_candidates"] == 1


def test_missing_scanned_set_is_an_error_not_a_fallback():
    """스캔 목록이 없으면 후보로 조용히 대체하지 않고 실패해야 한다."""
    d = _df([("i1", "a.py", 1.0)])
    with pytest.raises(ValueError, match="scanned"):
        evaluate_condition(d, {"i1": {"a.py"}}, scanned={}, condition="full", ks=(1,))
