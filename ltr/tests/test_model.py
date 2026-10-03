from __future__ import annotations

import json

import numpy as np
import pandas as pd
import pytest

from ltr.artifact import ArtifactSpec
from ltr.model import HAVE_LGBM, LambdaRankModel
from ltr.preprocessing import per_query_minmax


def test_artifact_round_trip(tmp_path):
    spec = ArtifactSpec(feature_columns=("bm25", "ppr"),
                        training_parameters={"num_leaves": 7})
    spec.save(tmp_path)

    loaded = ArtifactSpec.load(tmp_path)

    assert loaded == spec
    payload = json.loads((tmp_path / "artifact.json").read_text())
    assert payload["artifact_version"] == 1
    assert payload["feature_columns"] == ["bm25", "ppr"]


def test_per_query_minmax_preserves_nan_and_zeroes_constants():
    frame = pd.DataFrame({
        "instance_id": ["a", "a", "b", "b"],
        "x": [2.0, 4.0, 9.0, 9.0],
        "y": [np.nan, 3.0, 1.0, 5.0],
    })

    out = per_query_minmax(frame, ["x", "y"], "instance_id")

    assert out["x"].tolist() == [0.0, 1.0, 0.0, 0.0]
    assert np.isnan(out.loc[0, "y"])
    assert out.loc[1, "y"] == 0.0
    assert out.loc[2:, "y"].tolist() == [0.0, 1.0]


@pytest.mark.skipif(not HAVE_LGBM, reason="lightgbm is not installed")
def test_saved_model_prediction_round_trip(tmp_path):
    frame = pd.DataFrame({
        "instance_id": ["a"] * 4 + ["b"] * 4,
        "is_positive": [1, 0, 0, 0, 0, 1, 0, 0],
        "bm25": [4, 3, 2, 1, 1, 4, 3, 2],
        "ppr": [1, 2, 3, 4, 4, 1, 2, 3],
    })
    model = LambdaRankModel.fit(
        frame, ["bm25", "ppr"],
        params={"n_estimators": 5, "num_leaves": 3, "min_child_samples": 1,
                "n_jobs": 1})
    before = model.predict(frame)

    model.save(tmp_path)
    loaded = LambdaRankModel.load(tmp_path)
    after = loaded.predict(frame)

    np.testing.assert_allclose(after, before)
    assert loaded.spec.feature_columns == ("bm25", "ppr")
    assert (tmp_path / "model.txt").is_file()


def test_prediction_rejects_missing_feature():
    class StubBooster:
        def predict(self, _):  # pragma: no cover - validation runs first
            raise AssertionError

    model = LambdaRankModel(StubBooster(), ArtifactSpec(("bm25", "ppr")))
    with pytest.raises(ValueError, match="ppr"):
        model.predict(pd.DataFrame({"instance_id": ["a"], "bm25": [1.0]}))
