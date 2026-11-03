"""Acceptance tests for the future native `TelescodeHeadless rank` command.

These tests intentionally fail until the C++ inference command exists. They
must not skip a missing native runtime or substitute Python inference for it.
"""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

import numpy as np
import pandas as pd
import pytest

from ltr.model import LambdaRankModel
from ltr.preprocessing import model_matrix
from ltr.rank import main as python_rank_main, rank_files


ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/fixtures/ltr"
ARTIFACT = ROOT / "ltr/models/lambdarank-v1"
HEADLESS = Path(os.environ.get("TELESCODE_HEADLESS_BIN", ROOT / "build/TelescodeHeadless"))


def read_csv(path):
    return pd.read_csv(path, float_precision="round_trip",
                       converters={"instance_id": str, "file_id": str})


def run_rank(source, output, model=ARTIFACT):
    return subprocess.run([str(HEADLESS), "rank", "--model", str(model),
                           "--features", str(source), "--output", str(output)],
                          text=True, capture_output=True, timeout=30)


def copy_model(tmp_path):
    target = tmp_path / "model"
    target.mkdir()
    for file in ["artifact.json", "model.txt"]:
        shutil.copyfile(ARTIFACT / file, target / file)
    return target


def test_cpp_fixture_is_current_and_python_reproduces_it():
    manifest = json.loads((FIXTURE / "manifest.json").read_text())
    for relative, digest in manifest["sha256"].items():
        assert hashlib.sha256((ROOT / relative).read_bytes()).hexdigest() == digest, relative
    model = LambdaRankModel.load(ARTIFACT)
    frame = read_csv(FIXTURE / "input.csv")
    basename = frame.file_id.str.replace("\\", "/", regex=False).str.rsplit("/", n=1).str[-1]
    candidates = frame.loc[basename != "__init__.py"]
    matrix = model_matrix(candidates, list(model.spec.feature_columns),
                          model.spec.group_column, model.spec.normalization)
    prepared = pd.concat([candidates[["instance_id", "file_id"]], matrix], axis=1)
    pd.testing.assert_frame_equal(prepared.reset_index(drop=True),
                                  read_csv(FIXTURE / "prepared.csv"), check_dtype=False,
                                  check_exact=True)
    expected = rank_files(model, frame)
    pd.testing.assert_frame_equal(expected, read_csv(FIXTURE / "ranked.csv"),
                                  check_exact=True)
    assert len(frame) == 10 and len(expected) == 7
    tied = expected[expected.instance_id == "0001"]
    assert tied.file_id.tolist() == ["a.py", "z.py"]
    assert tied.score.nunique() == 1


def test_precision_fixture_detects_float32_inference():
    model = LambdaRankModel.load(ARTIFACT)
    frame = read_csv(FIXTURE / "boundary_input.csv")
    boundary = json.loads((FIXTURE / "boundary.json").read_text())
    feature, threshold = boundary["feature"], boundary["threshold"]
    values = frame.set_index("file_id")[feature]
    assert values["below.py"] == np.nextafter(threshold, -np.inf)
    assert values["at.py"] == threshold
    assert values["above.py"] == np.nextafter(threshold, np.inf)
    matrix = model_matrix(frame, list(model.spec.feature_columns),
                          model.spec.group_column, model.spec.normalization)
    pd.testing.assert_frame_equal(
        pd.concat([frame[["instance_id", "file_id"]], matrix], axis=1),
        read_csv(FIXTURE / "boundary_prepared.csv"), check_dtype=False, check_exact=True)
    expected = read_csv(FIXTURE / "boundary_ranked.csv")
    pd.testing.assert_frame_equal(rank_files(model, frame), expected, check_exact=True)
    # A deliberately wrong inference input precision must fail the same score
    # comparison used for the native runtime. This validates fixture sensitivity.
    bad_scores = pd.Series(model.booster.predict(matrix.astype(np.float32)),
                           index=frame.file_id).reindex(expected.file_id).to_numpy()
    with pytest.raises(AssertionError):
        np.testing.assert_allclose(bad_scores, expected.score, rtol=1e-12, atol=1e-12)


def test_none_fixture_preserves_every_raw_value_and_detects_renormalization():
    from dataclasses import replace
    loaded = LambdaRankModel.load(ARTIFACT)
    model = LambdaRankModel(loaded.booster, replace(loaded.spec, normalization="none"))
    frame = read_csv(FIXTURE / "input.csv")
    basename = frame.file_id.str.replace("\\", "/", regex=False).str.rsplit("/", n=1).str[-1]
    candidates = frame.loc[basename != "__init__.py"]
    expected = read_csv(FIXTURE / "none_prepared.csv")
    raw = candidates[["instance_id", "file_id", *model.spec.feature_columns]].reset_index(drop=True)
    assert expected.shape == (7, 21)
    pd.testing.assert_frame_equal(raw, expected, check_exact=True)
    ranked = read_csv(FIXTURE / "none_ranked.csv")
    pd.testing.assert_frame_equal(rank_files(model, frame), ranked, check_exact=True)
    bad = rank_files(loaded, frame).set_index(["instance_id", "file_id"]).score
    bad_scores = bad.reindex(pd.MultiIndex.from_frame(ranked[["instance_id", "file_id"]]))
    with pytest.raises(AssertionError):
        np.testing.assert_allclose(bad_scores, ranked.score, rtol=1e-12, atol=1e-12)


def test_python_cli_also_preserves_split_boundary_precision(tmp_path):
    output = tmp_path / "python_ranked.csv"
    assert python_rank_main(["--model", str(ARTIFACT), "--features",
                             str(FIXTURE / "boundary_input.csv"), "--output", str(output)]) == 0
    actual, expected = read_csv(output), read_csv(FIXTURE / "boundary_ranked.csv")
    pd.testing.assert_frame_equal(actual.drop(columns="score"), expected.drop(columns="score"))
    np.testing.assert_allclose(actual.score, expected.score, rtol=1e-12, atol=1e-12)


@pytest.mark.parametrize("prefix", ["", "boundary_"])
def test_cpp_rank_matches_python_scores_order_and_identifiers(tmp_path, prefix):
    output = tmp_path / "ranked.csv"
    before = {file: (ARTIFACT / file).read_bytes() for file in ["model.txt", "artifact.json"]}
    proc = run_rank(FIXTURE / f"{prefix}input.csv", output)
    assert proc.returncode == 0, proc.stderr
    actual, expected = read_csv(output), read_csv(FIXTURE / f"{prefix}ranked.csv")
    assert actual.columns.tolist() == ["instance_id", "file_id", "score", "rank"]
    pd.testing.assert_frame_equal(actual.drop(columns="score"), expected.drop(columns="score"))
    np.testing.assert_allclose(actual.score, expected.score, rtol=1e-12, atol=1e-12)
    for file, data in before.items():
        assert (ARTIFACT / file).read_bytes() == data


def test_cpp_rank_honors_none_normalization_for_scores_and_ranks(tmp_path):
    model = copy_model(tmp_path)
    path = model / "artifact.json"
    metadata = json.loads(path.read_text())
    metadata["normalization"] = "none"
    path.write_text(json.dumps(metadata))
    output = tmp_path / "ranked.csv"
    proc = run_rank(FIXTURE / "input.csv", output, model)
    assert proc.returncode == 0, proc.stderr
    actual, expected = read_csv(output), read_csv(FIXTURE / "none_ranked.csv")
    pd.testing.assert_frame_equal(actual.drop(columns="score"), expected.drop(columns="score"))
    np.testing.assert_allclose(actual.score, expected.score, rtol=1e-12, atol=1e-12)


@pytest.mark.parametrize("kind,diagnostic", [
    ("duplicate_header", "duplicate"),
    ("missing_feature", "missing"),
    ("duplicate_candidate", "duplicate"),
    ("empty_query", "query"),
    ("infinity", "finite"),
    ("only_initializers", "candidate"),
])
def test_cpp_rank_rejects_invalid_input_without_overwriting_output(tmp_path, kind, diagnostic):
    frame = read_csv(FIXTURE / "input.csv")
    if kind == "duplicate_header":
        frame = pd.concat([frame, pd.Series(999., index=frame.index, name="pagerank")], axis=1)
    elif kind == "missing_feature":
        frame = frame.drop(columns="pagerank")
    elif kind == "duplicate_candidate":
        frame = pd.concat([frame, frame.iloc[:1]])
    elif kind == "empty_query":
        frame.loc[0, "instance_id"] = ""
    elif kind == "infinity":
        frame.loc[0, "pagerank"] = np.inf
    elif kind == "only_initializers":
        frame = frame.iloc[[0]].assign(file_id="__init__.py")
    source, output = tmp_path / "features.csv", tmp_path / "ranked.csv"
    frame.to_csv(source, index=False)
    output.write_text("previous ranking\n")
    proc = run_rank(source, output)
    assert proc.returncode != 0
    assert output.read_text() == "previous ranking\n"
    assert diagnostic in proc.stderr.lower(), proc.stderr


def test_cpp_rank_rejects_metadata_order_mismatch(tmp_path):
    model = copy_model(tmp_path)
    path = model / "artifact.json"
    metadata = json.loads(path.read_text())
    metadata["feature_columns"].reverse()
    path.write_text(json.dumps(metadata))
    output = tmp_path / "ranked.csv"
    proc = run_rank(FIXTURE / "input.csv", output, model)
    assert proc.returncode != 0
    assert "feature" in proc.stderr.lower(), proc.stderr
    assert not output.exists()


@pytest.mark.parametrize("target", ["features", "model", "metadata"])
def test_cpp_rank_refuses_to_overwrite_inputs(tmp_path, target):
    model = copy_model(tmp_path)
    source = tmp_path / "features.csv"
    shutil.copyfile(FIXTURE / "input.csv", source)
    output = {"features": source, "model": model / "model.txt",
              "metadata": model / "artifact.json"}[target]
    before = output.read_bytes()
    proc = run_rank(source, output, model)
    assert proc.returncode != 0
    assert output.read_bytes() == before
    assert "overwrite" in proc.stderr.lower(), proc.stderr
