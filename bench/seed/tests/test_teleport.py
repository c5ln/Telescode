import math

import pytest

from bench.seed.teleport import TeleportError, build_teleport, validate_teleport


def test_sums_to_one_and_nonnegative():
    tv = build_teleport([("a.py", 3.0), ("b.py", 1.0)])
    validate_teleport(tv)
    assert math.isclose(tv.total, 1.0)
    assert all(w >= 0 for _, w in tv.pairs)
    assert math.isclose(dict(tv.pairs)["a.py"], 0.75)


def test_drops_nodes_missing_from_graph_and_reports_mass():
    tv = build_teleport([("a.py", 3.0), ("ghost.py", 1.0)], graph_file_ids={"a.py"})
    validate_teleport(tv, {"a.py"})
    assert tv.dropped_file_ids == ["ghost.py"]
    assert math.isclose(tv.dropped_mass, 0.25)
    assert math.isclose(tv.total, 1.0)


def test_all_zero_scores_fall_back_to_uniform():
    tv = build_teleport([("a.py", 0.0), ("b.py", 0.0)], graph_file_ids={"a.py", "b.py"})
    validate_teleport(tv, {"a.py", "b.py"})
    assert tv.fallback_uniform
    assert math.isclose(tv.total, 1.0)


def test_negative_score_rejected():
    with pytest.raises(TeleportError):
        build_teleport([("a.py", -1.0)])


def test_top_k_truncation():
    tv = build_teleport([(f"f{i}.py", float(i + 1)) for i in range(50)], top_k=5)
    assert len(tv.pairs) == 5
    assert math.isclose(tv.total, 1.0)


def test_file_id_normalized():
    tv = build_teleport([("./src/a.py", 1.0)])
    assert tv.pairs[0][0] == "src/a.py"


def test_validate_catches_unnormalized_sum():
    from bench.seed.teleport import TeleportVector
    with pytest.raises(TeleportError):
        validate_teleport(TeleportVector(pairs=[("a.py", 0.5), ("b.py", 0.4)]))
