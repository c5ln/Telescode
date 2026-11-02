from __future__ import annotations

import json
from pathlib import Path
import shutil

import numpy as np
import pandas as pd
import pytest

from ltr.artifact import ArtifactSpec
from ltr.model import HAVE_LGBM, LambdaRankModel
from ltr.rank import main, rank_files


class RecordingBooster:
    def predict(self, matrix):
        self.matrix = matrix.copy()
        return matrix.iloc[:, 0].fillna(0).to_numpy()


def test_candidates_precede_normalization_and_queries_are_independent():
    booster = RecordingBooster()
    model = LambdaRankModel(booster, ArtifactSpec(("bm25", "ppr")))
    frame = pd.DataFrame({
        "instance_id": ["a", "a", "a", "b", "b"],
        "file_id": ["pkg/__init__.py", "z.py", "b.py", "c.py", "a.py"],
        "ppr": [999, np.nan, 3, 7, 7],
        "bm25": [1000, 2, 4, 100, 100],
    }, index=[3, 3, 7, 9, 9])
    before = frame.copy(deep=True)
    out = rank_files(model, frame)
    assert list(booster.matrix.columns) == ["bm25", "ppr"]
    assert booster.matrix["bm25"].tolist() == [0, 1, 0, 0]
    np.testing.assert_equal(booster.matrix["ppr"].to_numpy(), [np.nan, 0, 0, 0])
    assert out["file_id"].tolist() == ["b.py", "z.py", "a.py", "c.py"]
    assert out["rank"].tolist() == [1, 2, 1, 2]
    pd.testing.assert_frame_equal(frame, before)


def test_windows_initializer_is_also_excluded():
    model = LambdaRankModel(RecordingBooster(), ArtifactSpec(("bm25",)))
    frame = pd.DataFrame({"instance_id": ["a", "a"],
                          "file_id": [r"pkg\__init__.py", "keep.py"], "bm25": [5, 1]})
    assert rank_files(model, frame)["file_id"].tolist() == ["keep.py"]


@pytest.mark.parametrize("change,error", [
    (lambda f: f.drop(columns="bm25"), "missing columns"),
    (lambda f: pd.concat([f, f]), "duplicate"),
    (lambda f: f.assign(file_id="__init__.py"), "no candidates"),
    (lambda f: f.assign(instance_id=None), "non-empty strings"),
    (lambda f: f.assign(bm25=np.inf), "finite or NaN"),
    (lambda f: f.iloc[:0], "empty"),
])
def test_invalid_inputs_fail_before_inference(change, error):
    class NoPrediction:
        def predict(self, _):
            pytest.fail("inference must not run")
    model = LambdaRankModel(NoPrediction(), ArtifactSpec(("bm25",)))
    frame = pd.DataFrame({"instance_id": ["a"], "file_id": ["f.py"], "bm25": [1.]})
    with pytest.raises(ValueError, match=error):
        rank_files(model, change(frame))


@pytest.mark.parametrize("field,value", [
    ("model_type", "other"), ("candidate_policy", "top20"),
    ("model_file", "other.txt"), ("group_column", ""),
    ("feature_columns", ("",)),
])
def test_unsupported_contracts_are_rejected(field, value):
    values = {"feature_columns": ("bm25",), field: value}
    with pytest.raises(ValueError):
        ArtifactSpec(**values)


@pytest.mark.parametrize("payload", [{}, [], {"feature_columns": "bm25"}])
def test_artifact_requires_an_ordered_feature_array(tmp_path, payload):
    (tmp_path / "artifact.json").write_text(json.dumps(payload))
    with pytest.raises(ValueError, match="feature_columns array"):
        ArtifactSpec.load(tmp_path)


ARTIFACT = Path(__file__).resolve().parents[1] / "models/lambdarank-v1"


@pytest.mark.skipif(not HAVE_LGBM, reason="lightgbm is not installed")
def test_real_model_cli_matches_reference_and_does_not_overwrite_inputs(tmp_path):
    model = LambdaRankModel.load(ARTIFACT)
    frame = pd.DataFrame({name: [0., 1., 2., 100.]
                          for name in model.spec.feature_columns})
    frame["instance_id"] = "0001"
    frame["file_id"] = ["a.py", "b.py", "c.py", "pkg/__init__.py"]
    source, output = tmp_path / "features.csv", tmp_path / "ranked.csv"
    frame.to_csv(source, index=False)
    assert main(["--model", str(ARTIFACT), "--features", str(source),
                 "--output", str(output)]) == 0
    actual = pd.read_csv(output, dtype={"instance_id": str})
    expected_scores = model.predict(frame.iloc[:3])
    expected = rank_files(model, frame)
    np.testing.assert_array_equal(expected.set_index("file_id").loc[
        ["a.py", "b.py", "c.py"], "score"].to_numpy(), expected_scores)
    pd.testing.assert_frame_equal(actual, expected)
    original = source.read_bytes()
    with pytest.raises(SystemExit):
        main(["--model", str(ARTIFACT), "--features", str(source), "--output", str(source)])
    assert source.read_bytes() == original


@pytest.mark.skipif(not HAVE_LGBM, reason="lightgbm is not installed")
def test_same_feature_count_but_wrong_order_is_rejected(tmp_path):
    shutil.copyfile(ARTIFACT / "model.txt", tmp_path / "model.txt")
    payload = json.loads((ARTIFACT / "artifact.json").read_text())
    payload["feature_columns"].reverse()
    (tmp_path / "artifact.json").write_text(json.dumps(payload))
    with pytest.raises(ValueError, match="names/order"):
        LambdaRankModel.load(tmp_path)
