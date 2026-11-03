"""Regenerate the C++ inference oracle using the released Python model.

Run from the repository root:
    bench/.venv/bin/python -m ltr.tests.generate_cpp_rank_fixture
This writes test data only; it never trains or updates model weights.
"""

from pathlib import Path
from dataclasses import replace
import hashlib
import json

import numpy as np
import pandas as pd

from ltr.model import LambdaRankModel
from ltr.preprocessing import model_matrix
from ltr.rank import rank_files


ROOT = Path(__file__).resolve().parents[2]
ARTIFACT = ROOT / "ltr/models/lambdarank-v1"
FIXTURE = ROOT / "tests/fixtures/ltr"


def write_expected(model, frame, prepared_name, ranked_name):
    basename = frame.file_id.str.replace("\\", "/", regex=False).str.rsplit("/", n=1).str[-1]
    candidates = frame.loc[basename != "__init__.py"].copy()
    matrix = model_matrix(candidates, list(model.spec.feature_columns),
                          model.spec.group_column, model.spec.normalization)
    prepared = pd.concat([candidates[["instance_id", "file_id"]], matrix], axis=1)
    prepared.to_csv(FIXTURE / prepared_name, index=False, float_format="%.17g")
    rank_files(model, frame).to_csv(FIXTURE / ranked_name, index=False, float_format="%.17g")
    return len(candidates)


def main():
    model = LambdaRankModel.load(ARTIFACT)
    columns = list(model.spec.feature_columns)
    rng = np.random.default_rng(20261102)
    rows = []

    def row(query, file, values):
        rows.append({"instance_id": query, "file_id": file,
                     **dict(zip(columns, values)), "unused_feature": 12345.})

    for index, file in enumerate(["z.py", "a.py", "pkg/a,quoted.py"]):
        values = rng.normal(size=len(columns)) * (index + 1)
        values[columns.index("bc")] = 7.  # query-local constant
        values[columns.index("str_match")] = 0.
        values[columns.index("ppr")] = [np.nan, .1, .4][index]
        values[columns.index("nbr_bm25_mean")] = np.nan  # entirely missing in this query
        row("NA", file, values)
    for file in ["pkg/__init__.py", r"pkg\__init__.py"]:
        row("NA", file, [1e9] * len(columns))  # excluded before extrema
    for file in ["z.py", "a.py"]:
        values = [3.] * len(columns)  # exact model-score tie, reversed file order
        values[columns.index("ppr")] = np.nan
        row("0001", file, values)
    for index, file in enumerate(['pkg/quote".py', "pkg/한글.py"]):
        values = rng.normal(size=len(columns)) * 100 + 500 * index
        row('질의,"case"', file, values)
    row('질의,"case"', "__init__.py", [-1e9] * len(columns))

    # Interleave groups and reverse feature columns to exercise grouping and
    # metadata ordering independently of incoming row/column order.
    frame = pd.DataFrame(rows).iloc[[0, 5, 7, 3, 1, 6, 8, 4, 2, 9]]
    frame = frame[["instance_id", "file_id", "unused_feature", *reversed(columns)]]
    FIXTURE.mkdir(parents=True, exist_ok=True)
    frame.to_csv(FIXTURE / "input.csv", index=False, float_format="%.17g")
    frame = pd.read_csv(FIXTURE / "input.csv", float_precision="round_trip",
                        converters={"instance_id": str, "file_id": str})
    candidate_rows = write_expected(model, frame, "prepared.csv", "ranked.csv")
    none_model = LambdaRankModel(model.booster, replace(model.spec, normalization="none"))
    write_expected(none_model, frame, "none_prepared.csv", "none_ranked.csv")

    # Use an actual split from the released model. Anchors 0 and 1 ensure
    # per-query min-max leaves the boundary and its adjacent doubles unchanged.
    split = model.booster.dump_model()["tree_info"][0]["tree_structure"]
    feature, threshold = columns[split["split_feature"]], float(split["threshold"])
    if not 0 < threshold < 1:
        raise ValueError("choose a model split strictly between 0 and 1 for the precision fixture")
    probes = [("high.py", 1.), ("above.py", np.nextafter(threshold, np.inf)),
              ("below.py", np.nextafter(threshold, -np.inf)),
              ("at.py", threshold), ("low.py", 0.)]
    boundary = pd.DataFrame([
        {"instance_id": "precision", "file_id": file,
         **{c: value if c == feature else 0. for c in columns}}
        for file, value in probes
    ])[["instance_id", "file_id", *reversed(columns)]]
    boundary.to_csv(FIXTURE / "boundary_input.csv", index=False, float_format="%.17g")
    boundary = pd.read_csv(FIXTURE / "boundary_input.csv", float_precision="round_trip",
                           converters={"instance_id": str, "file_id": str})
    write_expected(model, boundary, "boundary_prepared.csv", "boundary_ranked.csv")
    (FIXTURE / "boundary.json").write_text(
        json.dumps({"feature": feature, "threshold": threshold}, indent=2) + "\n")
    files = [ARTIFACT / "artifact.json", ARTIFACT / "model.txt",
             *[FIXTURE / name for name in [
                 "input.csv", "prepared.csv", "ranked.csv", "none_prepared.csv", "none_ranked.csv",
                 "boundary_input.csv", "boundary_prepared.csv", "boundary_ranked.csv", "boundary.json"]]]
    manifest = {"generator": "python -m ltr.tests.generate_cpp_rank_fixture",
                "input_rows": len(frame), "candidate_rows": candidate_rows,
                "boundary_rows": len(boundary),
                "feature_columns": columns,
                "sha256": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                           for p in files}}
    (FIXTURE / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
