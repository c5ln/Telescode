"""`bench/ltr/` 회귀 테스트.

지표 자체는 `bench/metrics/test_ranking.py`가 지킨다. 여기서 지키는 것은 **파이프라인이
조용히 틀리는 자리**다. 전부 파이프라인이 정상적으로 완주하면서 그럴듯한 틀린 숫자를
내놓는 종류의 버그다.

    bench/.venv/bin/python -m pytest bench/ltr/test_ltr.py -q
"""

from __future__ import annotations

import numpy as np
import pandas as pd
import pytest

from bench.ltr.data import (join_bm25, load_segments, per_instance_minmax,
                            usable_features)
from bench.ltr.fusion import rrf, with_external
from bench.ltr.model import make_folds
from bench.ltr.size_control import size_matched_comparison
from bench.schema import C


def _frame(rows):
    return pd.DataFrame(rows)


# ── fold 배정 ────────────────────────────────────────────────────────────

def test_folds_are_instance_level_and_balanced():
    """무결성 규칙 §1. 한 인스턴스가 두 fold에 걸치면 그 자리가 곧 누수다."""
    ids = [f"i{n}" for n in range(23)]
    f = make_folds(ids, n_folds=5)
    assert set(f.mapping) == set(ids)
    sizes = pd.Series(list(f.mapping.values())).value_counts()
    # 균등 배정: 가장 큰 fold와 가장 작은 fold 차이가 1 이하
    assert sizes.max() - sizes.min() <= 1


def test_folds_are_deterministic():
    """같은 seed면 같은 배정. 실행마다 흔들리면 ablation 비교가 무의미해진다."""
    ids = [f"i{n}" for n in range(30)]
    assert make_folds(ids, 5, seed=1).mapping == make_folds(ids, 5, seed=1).mapping
    assert make_folds(ids, 5, seed=1).mapping != make_folds(ids, 5, seed=2).mapping


# ── 인스턴스 내 정규화 ───────────────────────────────────────────────────

def test_minmax_is_within_instance():
    """인스턴스 A와 B의 스케일이 달라도 각각 0~1로 간다."""
    df = _frame([
        {C.instance_id: "a", "x": 0.0}, {C.instance_id: "a", "x": 10.0},
        {C.instance_id: "b", "x": 1000.0}, {C.instance_id: "b", "x": 2000.0},
    ])
    out = per_instance_minmax(df, ["x"])["x"].tolist()
    assert out == [0.0, 1.0, 0.0, 1.0]


def test_minmax_constant_instance_gives_zero():
    df = _frame([{C.instance_id: "a", "x": 5.0}, {C.instance_id: "a", "x": 5.0}])
    assert per_instance_minmax(df, ["x"])["x"].tolist() == [0.0, 0.0]


def test_minmax_preserves_nan():
    """결측이 0(=최솟값)으로 둔갑하면 LightGBM의 결측 처리가 통째로 무력화된다."""
    df = _frame([
        {C.instance_id: "a", "x": 1.0}, {C.instance_id: "a", "x": np.nan},
        {C.instance_id: "b", "x": np.nan}, {C.instance_id: "b", "x": np.nan},
    ])
    out = per_instance_minmax(df, ["x"])["x"]
    assert out.iloc[0] == 0.0
    assert out.iloc[1:].isna().all()


# ── 사용 불가 피처 제거 ──────────────────────────────────────────────────

def test_usable_features_drops_allnan_and_constant():
    df = _frame([{"a": 1.0, "b": np.nan, "c": 3.0},
                 {"a": 2.0, "b": np.nan, "c": 3.0}])
    keep, dropped = usable_features(df, ["a", "b", "c"])
    assert keep == ["a"]
    assert any("b" in d for d in dropped) and any("c" in d for d in dropped)


# ── BM25 조인 ────────────────────────────────────────────────────────────

def test_join_bm25_rejects_bad_match_rate(tmp_path):
    """조인 실패는 NaN을 남기고, NaN은 최하위로 밀려 '검색이 쓸모없다'가 된다."""
    feats = _frame([{C.instance_id: "i", C.file_id: f"f{n}.py",
                     C.bm25: np.nan, C.bm25_rank: np.nan} for n in range(10)])
    seed = _frame([{C.instance_id: "i", C.file_id: "OTHER.py",
                    C.bm25: 1.0, C.bm25_rank: 1}])
    p = tmp_path / "seed.csv"
    seed.to_csv(p, index=False)
    with pytest.raises(SystemExit, match="매칭률"):
        join_bm25(feats, p)


def test_join_bm25_is_noop_when_already_filled(tmp_path):
    feats = _frame([{C.instance_id: "i", C.file_id: "f.py",
                     C.bm25: 2.0, C.bm25_rank: 1}])
    p = tmp_path / "seed.csv"
    _frame([{C.instance_id: "i", C.file_id: "f.py",
             C.bm25: 99.0, C.bm25_rank: 9}]).to_csv(p, index=False)
    out, msg = join_bm25(feats, p)
    assert out[C.bm25].tolist() == [2.0]
    assert "생략" in msg


# ── 구간 라벨 ────────────────────────────────────────────────────────────

def test_segments_only_use_gold_rows(tmp_path):
    """비gold 행이 구간에 섞이면 n_gold가 부풀어 구간 크기 보고가 거짓이 된다."""
    ov = _frame([
        {C.instance_id: "i", C.file_id: "g1.py", "condition": "full",
         C.is_positive: 1, "vocab_overlap": 0},
        {C.instance_id: "i", C.file_id: "g2.py", "condition": "full",
         C.is_positive: 1, "vocab_overlap": 1},
        {C.instance_id: "i", C.file_id: "x.py", "condition": "full",
         C.is_positive: 0, "vocab_overlap": 0},
    ])
    p = tmp_path / "ov.csv"
    ov.to_csv(p, index=False)
    feats = _frame([{C.instance_id: "i", C.file_id: f}
                    for f in ("g1.py", "g2.py", "x.py")])
    seg, _ = load_segments(feats, p, "full")
    assert seg["non_overlap"] == {"i": {"g1.py"}}
    assert seg["overlap"] == {"i": {"g2.py"}}


def test_segments_drop_instances_absent_from_matrix(tmp_path):
    ov = _frame([{C.instance_id: "gone", C.file_id: "g.py", "condition": "full",
                  C.is_positive: 1, "vocab_overlap": 0},
                 {C.instance_id: "here", C.file_id: "h.py", "condition": "full",
                  C.is_positive: 1, "vocab_overlap": 0}])
    p = tmp_path / "ov.csv"
    ov.to_csv(p, index=False)
    feats = _frame([{C.instance_id: "here", C.file_id: "h.py"}])
    seg, _ = load_segments(feats, p, "full")
    assert seg["non_overlap"] == {"here": {"h.py"}}


# ── RRF ──────────────────────────────────────────────────────────────────

def test_rrf_matches_hand_calculation():
    """손 계산 fixture. k=60, 두 랭커.

    a: x=3(1위) y=1(2위) → 1/61 + 1/62
    b: x=2(2위) y=2(1위) → 1/62 + 1/61   (a와 같아야 한다)
    c: x=1(3위) y=0(3위) → 1/63 + 1/63
    """
    df = _frame([
        {C.instance_id: "i", C.file_id: "a", "x": 3.0, "y": 1.0},
        {C.instance_id: "i", C.file_id: "b", "x": 2.0, "y": 2.0},
        {C.instance_id: "i", C.file_id: "c", "x": 1.0, "y": 0.0},
    ])
    out = rrf(df, [("x", True), ("y", True)], k=60).set_index(C.file_id)["rrf"]
    assert out["a"] == pytest.approx(1 / 61 + 1 / 62)
    assert out["b"] == pytest.approx(1 / 62 + 1 / 61)
    assert out["c"] == pytest.approx(2 / 63)


def test_rrf_respects_direction():
    """작을수록 좋은 컬럼에 부호를 안 뒤집으면 랭킹이 정확히 거꾸로 나온다."""
    df = _frame([{C.instance_id: "i", C.file_id: "a", "r": 1.0},
                 {C.instance_id: "i", C.file_id: "b", "r": 2.0}])
    lo = rrf(df, [("r", False)], k=60).set_index(C.file_id)["rrf"]
    assert lo["a"] > lo["b"]


def test_with_external_rejects_missing_keys():
    feats = _frame([{C.instance_id: "i", C.file_id: "a"},
                    {C.instance_id: "i", C.file_id: "b"}])
    ext = _frame([{C.instance_id: "i", C.file_id: "a", "s": 1.0}])
    with pytest.raises(SystemExit, match="결측"):
        with_external(feats, ext, "s")


# ── ablation 구성 ────────────────────────────────────────────────────────

def _ds_with(cols):
    from bench.ltr.data import Dataset
    return Dataset(features=pd.DataFrame(), gold_sets={}, scanned={},
                   feature_columns=list(cols))


def test_ablation_separates_ppr_from_global_graph():
    """graph 그룹을 통째로만 다루면 '목적 조건부 전파'가 전역 지표에 묻힌다.

    `-ppr`은 ppr **하나만** 빠져야 하고, `graph global only`에는 ppr이 **없어야**
    한다. 둘 중 하나라도 어긋나면 PPR 기여 측정이 통째로 다른 것을 잰다.
    """
    from bench.ltr.run_ltr import ablation_configs
    cfgs = dict(ablation_configs(_ds_with(
        [C.pagerank, C.bc, C.ppr, C.complexity, C.bm25])))

    assert C.ppr not in cfgs["-ppr (목적 조건부만 제거)"]
    assert set(cfgs["-ppr (목적 조건부만 제거)"]) == {
        C.pagerank, C.bc, C.complexity, C.bm25}
    assert cfgs["ppr only"] == [C.ppr]
    assert C.ppr not in cfgs["graph global only (ppr 제외)"]
    assert C.ppr in cfgs["graph only"]


def test_ablation_omits_ppr_arms_when_ppr_unusable():
    """ppr이 전부 NaN이던 시기에는 이 조건들이 아예 없어야 한다.

    빈 컬럼으로 만든 `ppr only`는 학습할 것이 없는데도 표에는 한 줄이 생겨
    '측정했다'는 착시를 만든다.
    """
    from bench.ltr.run_ltr import ablation_configs
    labels = [l for l, _ in ablation_configs(
        _ds_with([C.pagerank, C.bc, C.complexity, C.bm25]))]
    assert not [l for l in labels if "ppr" in l]


# ── 크기 매칭 ────────────────────────────────────────────────────────────

def test_size_matching_does_not_reuse_controls():
    """대조군 재사용은 유효 표본을 부풀린다. gold 2개면 서로 다른 대조군 2개여야 한다."""
    df = _frame([
        {C.instance_id: "i", C.file_id: "g1", C.is_positive: 1,
         C.logical_loc: 100.0, C.complexity: 0.9},
        {C.instance_id: "i", C.file_id: "g2", C.is_positive: 1,
         C.logical_loc: 101.0, C.complexity: 0.8},
        {C.instance_id: "i", C.file_id: "c1", C.is_positive: 0,
         C.logical_loc: 100.0, C.complexity: 0.2},
        {C.instance_id: "i", C.file_id: "c2", C.is_positive: 0,
         C.logical_loc: 400.0, C.complexity: 0.3},
    ])
    pairs, _ = size_matched_comparison(df)
    assert len(pairs) == 2
    assert pairs["ctrl_file"].nunique() == 2


def test_size_matching_reports_gap():
    """짝이 멀면 '크기 통제'가 아니다. rel_gap이 그 사실을 드러내야 한다."""
    df = _frame([
        {C.instance_id: "i", C.file_id: "g", C.is_positive: 1,
         C.logical_loc: 100.0, C.complexity: 0.9},
        {C.instance_id: "i", C.file_id: "c", C.is_positive: 0,
         C.logical_loc: 1000.0, C.complexity: 0.1},
    ])
    pairs, summary = size_matched_comparison(df)
    assert pairs["rel_gap"].iloc[0] == pytest.approx(0.9)
    assert summary.loc[summary["set"] == "rel_gap<=0.2", "n_pairs"].iloc[0] == 0
