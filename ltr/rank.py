"""Reference file ranking from computed features, without benchmark dependencies.

This is the Python oracle for the future C++ runtime. Feature generation is a
separate step: callers supply one row per (query, file) with raw feature values.
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import numpy as np
import pandas as pd

from .model import LambdaRankModel


def rank_files(model: LambdaRankModel, frame: pd.DataFrame) -> pd.DataFrame:
    """Exclude initializers, normalize surviving candidates, score, then rank.

    Results contain the artifact's group column, file_id, score, and a 1-based
    rank. Tied scores use ascending file_id so the C++ runtime can reproduce
    their order. Missing feature *values* remain NaN; missing columns fail.
    """
    group = model.spec.group_column
    required = [group, "file_id", *model.spec.feature_columns]
    missing = [c for c in required if c not in frame.columns]
    if missing:
        raise ValueError(f"ranking input is missing columns: {missing}")
    if not frame.columns.is_unique:
        raise ValueError("ranking input contains duplicate column names")
    if group in {"file_id", "score", "rank"}:
        raise ValueError(f"group column conflicts with ranking output: {group}")
    if model.spec.candidate_policy != "all_no_init":
        raise ValueError(f"unsupported candidate policy: {model.spec.candidate_policy}")
    if frame.empty:
        raise ValueError("ranking input is empty")
    for col in [group, "file_id"]:
        if not frame[col].map(lambda v: isinstance(v, str) and bool(v.strip())).all():
            raise ValueError(f"{col} must contain non-empty strings")
    if frame.duplicated([group, "file_id"]).any():
        raise ValueError("duplicate (query, file_id) candidates")

    # Match the benchmark policy, before computing per-query extrema. An
    # initializer's high score must not change other files' normalized inputs.
    basename = frame["file_id"].str.replace("\\", "/", regex=False).str.rsplit("/", n=1).str[-1]
    candidates = frame.loc[basename != "__init__.py"].copy()
    lost = set(frame[group]) - set(candidates[group])
    if lost:
        raise ValueError(f"no candidates after excluding __init__.py: {sorted(lost)}")
    raw = candidates[list(model.spec.feature_columns)].to_numpy(dtype=float)
    if np.isinf(raw).any():
        raise ValueError("feature values must be finite or NaN")
    scores = np.asarray(model.predict(candidates), dtype=float)
    if scores.shape != (len(candidates),) or not np.isfinite(scores).all():
        raise ValueError("model returned invalid ranking scores")
    out = candidates[[group, "file_id"]].copy()
    out["score"] = scores
    out = out.sort_values([group, "score", "file_id"],
                          ascending=[True, False, True], kind="stable")
    out["rank"] = out.groupby(group, sort=False).cumcount() + 1
    return out.reset_index(drop=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True, type=Path, help="model artifact directory")
    parser.add_argument("--features", required=True, type=Path, help="raw feature CSV")
    parser.add_argument("--output", required=True, type=Path, help="ranked file CSV")
    args = parser.parse_args(argv)
    protected = [args.features, args.model / "model.txt", args.model / "artifact.json"]
    for path in protected:
        if (args.output.resolve() == path.resolve() or
                (args.output.exists() and path.exists() and args.output.samefile(path))):
            parser.error("--output would overwrite an input or model artifact")
    try:
        model = LambdaRankModel.load(args.model)
        with args.features.open(encoding="utf-8-sig", newline="") as features:
            # pandas renames duplicate headers before returning a DataFrame,
            # so validate the original CSV header before that can happen.
            header = next((row for row in csv.reader(features) if row), [])
            if len(set(header)) != len(header):
                raise ValueError("ranking input contains duplicate CSV column names")
            features.seek(0)
            # Converters preserve identifiers such as "NA" and "0001".
            # Numeric feature columns still use pandas' normal NaN handling.
            frame = pd.read_csv(features,
                                converters={model.spec.group_column: str, "file_id": str})
        out = rank_files(model, frame)
        out.to_csv(args.output, index=False)
    except (ValueError, TypeError, OSError, RuntimeError, csv.Error) as exc:
        parser.exit(1, f"ranking failed: {exc}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
