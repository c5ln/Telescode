"""Expected behavior for the three issues found in the stage 1 review."""

from pathlib import Path

import numpy as np
import pandas as pd
import pytest

from ltr.model import HAVE_LGBM, LambdaRankModel
from ltr.rank import main, rank_files


ARTIFACT = Path(__file__).resolve().parents[1] / "models/lambdarank-v1"
pytestmark = pytest.mark.skipif(not HAVE_LGBM, reason="lightgbm is not installed")


def feature_frame():
    model = LambdaRankModel.load(ARTIFACT)
    frame = pd.DataFrame({name: [0., 1.] for name in model.spec.feature_columns})
    frame["instance_id"] = "query"
    frame["file_id"] = ["a.py", "b.py"]
    return model, frame


def test_cli_rejects_duplicate_csv_headers_without_overwriting_output(tmp_path, capsys):
    _, frame = feature_frame()
    # Two identically named columns contain different values: accepting the
    # first silently would rank files using an ambiguous input contract.
    frame = pd.concat([frame, pd.Series([999., 888.], name="pagerank")], axis=1)
    source, output = tmp_path / "features.csv", tmp_path / "ranked.csv"
    frame.to_csv(source, index=False)
    original_output = "previous ranking\n"
    output.write_text(original_output)

    with pytest.raises(SystemExit) as error:
        main(["--model", str(ARTIFACT), "--features", str(source),
              "--output", str(output)])

    assert error.value.code == 1
    assert "duplicate" in capsys.readouterr().err.lower()
    assert output.read_text() == original_output


def test_cli_preserves_na_query_id_and_missing_feature_values(tmp_path):
    model, frame = feature_frame()
    frame["instance_id"] = "NA"
    frame.loc[0, "ppr"] = np.nan
    source, output = tmp_path / "features.csv", tmp_path / "ranked.csv"
    frame.to_csv(source, index=False)
    expected = rank_files(model, frame)

    assert main(["--model", str(ARTIFACT), "--features", str(source),
                 "--output", str(output)]) == 0

    actual = pd.read_csv(output, keep_default_na=False)
    pd.testing.assert_frame_equal(actual, expected)


def test_fit_rejects_feature_names_that_lightgbm_would_rename():
    # Reject unsupported names at the training boundary, rather than creating
    # an artifact whose model and metadata disagree and cannot be reloaded.
    frame = pd.DataFrame({
        "instance_id": ["query"] * 4,
        "is_positive": [1, 0, 0, 0],
        "has space": [4., 3., 2., 1.],
    })

    with pytest.raises(ValueError, match="feature"):
        LambdaRankModel.fit(
            frame, ["has space"],
            params={"n_estimators": 1, "num_leaves": 2,
                    "min_child_samples": 1, "n_jobs": 1})
