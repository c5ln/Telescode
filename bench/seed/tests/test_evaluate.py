"""평가 배관 테스트 — 지표 자체가 아니라 **연결**을 검증한다.

지표 정확성은 `bench/metrics/` 소유이고 그쪽 테스트가 담당한다.
"""
import pandas as pd

from bench.schema import C
from bench.seed.evaluate import evaluate_condition, metrics_available


def _df(rows):
    return pd.DataFrame(rows, columns=[C.instance_id, C.file_id, C.bm25])


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
    r = evaluate_condition(_df([("i1", "a.py", 9.0), ("i1", "b.py", 1.0)]),
                           {"i1": {"a.py"}}, condition="full", ks=(1, 2))
    s = r.summary
    assert s["recall@1"] == 1.0 and s["mrr"] == 1.0 and s["reachable_recall_ceiling"] == 1.0


def test_tiebreak_is_pessimistic_end_to_end():
    # 동점이면 gold가 뒤로 가야 하므로 recall@1 == 0
    r = evaluate_condition(_df([("i1", "a.py", 5.0), ("i1", "b.py", 5.0)]),
                           {"i1": {"a.py"}}, condition="full", ks=(1, 2))
    assert r.summary["recall@1"] == 0.0
    assert r.summary["recall@2"] == 1.0


def test_gold_missing_from_candidates_lowers_ceiling():
    r = evaluate_condition(_df([("i1", "a.py", 1.0)]),
                           {"i1": {"a.py", "ghost.py"}}, condition="full", ks=(1,))
    assert r.summary["reachable_recall_ceiling"] == 0.5
    assert r.summary["recall@1"] == 0.5, "분모는 gold 전체다 (스캔에 없는 것 포함)"
