"""`code_lexical` / `edges` 회귀 테스트.

    bench/.venv/bin/python -m pytest bench/features/test_code_lexical.py -q
"""

from __future__ import annotations

import pandas as pd

from bench.features.code_lexical import (def_match_scores, defined_names,
                                         literal_segments, module_names,
                                         neighbor_aggregates, normalize_text,
                                         query_identifiers, str_match_scores,
                                         symbol_scores, used_identifiers)
from bench.features.edges import degree_mismatches, repo_of
from bench.schema import C


def test_defined_names_def_class_async_and_no_dunder():
    src = "class Dataset:\n    def __init__(self): ...\n    async def load_all(self): ...\ndef ab(): ...\n"
    assert defined_names(src) == {"dataset", "load_all"}


def test_usage_is_not_definition():
    assert defined_names("x = Dataset()\nfrom a import Dataset\n") == frozenset()


def test_query_identifiers_drop_stopwords_and_short():
    assert query_identifiers("the Dataset.to_netcdf is ok") == {"dataset", "to_netcdf"}


def test_def_match_rewards_rare_definition():
    defs = [frozenset({"dataset", "helper"}), frozenset({"helper"}), frozenset()]
    s = def_match_scores(defs, frozenset({"dataset", "helper"}))
    assert s[0] > s[1] > s[2] == 0.0


def test_literal_segments_split_on_placeholders_and_skip_docstrings():
    src = ('"""cannot align objects with join exact here"""\n'
           'raise ValueError(f"cannot align objects {name} with join=\'exact\'")\n'
           'x = "short"\n')
    segs = literal_segments(src)
    assert "cannot align objects" in segs
    assert all("short" not in s for s in segs)
    assert "cannot align objects with join exact here" not in segs


def test_percent_format_is_split():
    segs = literal_segments('msg = "invalid frequency %r for the index" % f\n')
    assert segs == {"invalid frequency", "for the index"}


def test_str_match_substring_and_idf():
    segs = [frozenset({"cannot align objects"}), frozenset({"cannot align objects", "other message text"}),
            frozenset({"other message text"})]
    q = normalize_text("ValueError: Cannot   align objects with join='exact'")
    s = str_match_scores(segs, q)
    assert s[0] > 0 and s[0] == s[1] and s[2] == 0.0


def test_neighbor_aggregates_undirected_and_isolated_zero():
    mx, mean = neighbor_aggregates(["a", "b", "c", "d"],
                                   {"a": 1.0, "b": 3.0, "c": 5.0, "d": 9.0},
                                   [("a", "b"), ("c", "a")])
    assert mx == {"a": 5.0, "b": 1.0, "c": 1.0, "d": 0.0}
    assert mean["a"] == 4.0 and mean["d"] == 0.0


def test_module_names_top_level_only():
    src = ("OPTIONS = {}\nDISPLAY_STYLE: str = 'html'\n__all__ = []\n"
           "if x == 1:\n    INNER = 2\nx == y\n")
    assert module_names(src) == {"options", "display_style"}


def test_used_identifiers_ignore_comments_and_docstrings():
    src = '"""map_blocks is described here"""\n# apply_ufunc in a comment\nresult = map_blocks(f)\n'
    used = used_identifiers(src)
    assert "map_blocks" in used and "apply_ufunc" not in used and "described" not in used


def test_symbol_scores_use_and_prop():
    """b와 c는 a가 정의한 심볼을 쓴다. b만 a와 간선으로 연결돼 prop을 받는다.
    a는 정의 파일이라 use/prop 모두 0, d는 심볼을 안 써서 0."""
    fids = ["a", "b", "c", "d"]
    defs = [frozenset({"map_blocks"}), frozenset(), frozenset(), frozenset()]
    used = [frozenset({"map_blocks"}), frozenset({"map_blocks"}),
            frozenset({"map_blocks", "other"}), frozenset({"other"})]
    use, prop = symbol_scores(defs, used, fids, frozenset({"map_blocks", "other"}),
                              [("b", "a")])
    assert use[0] == 0.0 and use[1] > 0 and use[1] == use[2] and use[3] == 0.0
    assert prop[1] == use[1] and prop[2] == 0.0 and prop[0] == 0.0


def test_symbol_needs_a_definition_in_repo():
    """정의가 없는 이슈 단어(`other`)는 심볼로 치지 않는다."""
    use, _ = symbol_scores([frozenset(), frozenset()],
                           [frozenset({"other"}), frozenset()],
                           ["a", "b"], frozenset({"other"}), [])
    assert use == [0.0, 0.0]


def test_repo_of():
    assert repo_of("pydata__xarray-3095") == "pydata/xarray"
    assert repo_of("pytest-dev__pytest-10051") == "pytest-dev/pytest"


def test_degree_mismatches_detects_difference():
    rows = pd.DataFrame({C.file_id: ["a", "b"], C.in_deg: [0, 1], C.out_deg: [1, 0]})
    assert degree_mismatches({("a", "b")}, rows) == 0
    assert degree_mismatches({("b", "a")}, rows) == 2
