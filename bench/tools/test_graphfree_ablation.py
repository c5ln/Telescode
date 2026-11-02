"""graphfree_ablation의 백분위·분해가 Scoring.cpp 규칙과 같은지 손으로 계산한 값으로 확인."""

import numpy as np
import pandas as pd

from bench.collect.pipeline import seed_provenance
from bench.tools.graphfree_ablation import NODEG, add_nodeg, percentile_rank


def test_percentile_rank_rules():
    assert percentile_rank(np.array([5.0])).tolist() == [0.0]           # N=1
    assert percentile_rank(np.array([2.0, 2.0])).tolist() == [0.5, 0.5]  # 전부 같음
    # strictly-lower 개수 / (N-1): 동점은 같은 낮은 값
    assert percentile_rank(np.array([3.0, 1.0, 3.0, 2.0])).tolist() == [2 / 3, 0.0, 2 / 3, 1 / 3]


def _frame(complexity):
    return pd.DataFrame({
        "instance_id": ["i"] * 3, "file_id": ["a.py", "b.py", "gen.py"],
        "is_generated": [0, 0, 1],
        "max_cc": [1, 3, 9], "avg_cc": [1.0, 1.0, 9.0],
        "max_depth": [2, 1, 9], "avg_depth": [0.5, 0.5, 9.0],
        "logical_loc": [10, 20, 999],
        "in_deg": [0, 4, 7], "out_deg": [1, 0, 7],
        "complexity": complexity,
    })


def test_nodeg_decomposition_and_generated_rows():
    # 생성 파일 제외 2개: pct는 0 또는 1
    #   a: cc 0, nest 1, loc 0, in 0, out 1 → nodeg 0.20,  full 0.20+0.15 = 0.35
    #   b: cc 1, nest 0, loc 1, in 1, out 0 → nodeg 0.45,  full 0.45+0.20 = 0.65
    df, check = add_nodeg(_frame([0.35, 0.65, 0.0]))
    assert np.allclose(df[NODEG], [0.20, 0.45, 0.0])
    assert check["max_abs_identity_residual"] < 1e-12
    assert check["generated_complexity_all_zero"]
    _, bad = add_nodeg(_frame([0.35, 0.60, 0.0]))
    assert bad["max_abs_identity_residual"] > 0.04


def test_seed_provenance(tmp_path):
    assert seed_provenance(None, "embed", 2, 1.0) == {}
    f = tmp_path / "s.csv"
    f.write_text("instance_id,file_id,embed\n")
    p = seed_provenance(str(f), "embed", 2, 1.0)
    assert p["seed_topk"] == 2 and p["seed_power"] == 1.0 and len(p["seed_md5"]) == 32
