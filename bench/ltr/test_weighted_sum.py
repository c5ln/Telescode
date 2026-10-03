"""`bench/ltr/weighted_sum.py` 회귀 테스트.

    bench/.venv/bin/python -m pytest bench/ltr/test_weighted_sum.py -q
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from bench.ltr.weighted_sum import FORMS, make_ctx, score_frame, select_params
from bench.schema import C

FORM = {f.key: f for f in FORMS}


def _ctx():
    f = pd.DataFrame({
        C.instance_id: ["i"] * 4,
        C.file_id: ["a", "b", "c", "d"],
        C.bm25: [4.0, 3.0, 2.0, 0.0],
        "p": [0.0, 1.0, 10.0, 100.0],
    })
    return make_ctx(f, ["p"])


def test_select_params_uses_train_only():
    """test 인스턴스가 다른 설정을 강하게 선호해도 선택은 train만 따른다 (§4)."""
    tbl = pd.DataFrame({0: [0.1, 0.1, 1.0], 1: [0.9, 0.9, 0.0]},
                       index=["tr1", "tr2", "te"])
    assert select_params(tbl, ["tr1", "tr2"]) == 1


def test_select_params_ties_prefer_first():
    tbl = pd.DataFrame({0: [0.5], 1: [0.5], 2: [0.5]}, index=["a"])
    assert select_params(tbl, ["a"]) == 0


def test_every_grid_starts_with_bm25_equivalent():
    """그리드 첫 설정은 BM25 순서를 그대로 내야 한다 — 동점 규칙의 전제."""
    ctx = _ctx()
    for form in FORMS:
        s = score_frame(ctx, form, "p", form.grid[0])["wsum"].to_numpy()
        assert list(np.argsort(-s, kind="stable")[:3]) == [0, 1, 2], form.key


def test_linear_endpoints():
    ctx = _ctx()
    s = score_frame(ctx, FORM["linear"], "p", {"alpha": 0.0})["wsum"].to_numpy()
    assert np.argmax(s) == 3


def test_mult_zero_bm25_stays_bottom():
    """곱셈 prior: BM25가 0인 파일은 권위가 최대여도 올라오지 않는다."""
    ctx = _ctx()
    s = score_frame(ctx, FORM["mult"], "p", {"beta": 4.0, "eps": 0.01})["wsum"].to_numpy()
    assert s[3] == 0.0 and s[3] <= s.min()


def test_cascade_never_lifts_outside_candidates():
    """상위 N 밖 파일은 구조 점수가 커도 후보 위로 못 올라간다."""
    ctx = _ctx()
    s = score_frame(ctx, FORM["cascade"], "p", {"n": 2, "w": 0.0})["wsum"].to_numpy()
    assert min(s[0], s[1]) > max(s[2], s[3])
    assert s[1] > s[0]          # 후보 안에서는 구조 점수 순
    assert s[2] > s[3]          # 후보 밖은 BM25 순
