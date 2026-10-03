"""Preprocessing shared by final training and inference."""

from __future__ import annotations

import numpy as np
import pandas as pd


def per_query_minmax(frame: pd.DataFrame, feature_columns: list[str],
                     group_column: str) -> pd.DataFrame:
    """Min-max normalize features independently inside each ranking query.

    Constant columns become zero. Missing values remain missing so LightGBM's
    native missing-value handling is preserved.
    """
    required = [group_column, *feature_columns]
    missing = [c for c in required if c not in frame.columns]
    if missing:
        raise ValueError(f"model input is missing columns: {missing}")

    values = frame[feature_columns].astype(float)
    grouped = frame.groupby(group_column, sort=False)[feature_columns]
    low = grouped.transform("min")
    high = grouped.transform("max")
    span = (high - low).to_numpy()
    raw = values.to_numpy()
    with np.errstate(invalid="ignore", divide="ignore"):
        normalized = np.where(span > 0, (raw - low.to_numpy()) / span, 0.0)
    normalized = np.where(np.isnan(raw), np.nan, normalized)
    return pd.DataFrame(normalized, columns=feature_columns, index=frame.index)


def model_matrix(frame: pd.DataFrame, feature_columns: list[str],
                 group_column: str, normalization: str) -> pd.DataFrame:
    """Build a model matrix in the artifact's exact feature order."""
    if normalization == "per_query_minmax":
        return per_query_minmax(frame, feature_columns, group_column)
    if normalization == "none":
        missing = [c for c in feature_columns if c not in frame.columns]
        if missing:
            raise ValueError(f"model input is missing features: {missing}")
        return frame[feature_columns].astype(float)
    raise ValueError(f"unsupported normalization: {normalization}")
