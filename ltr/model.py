"""Train, persist, load, and run the production LambdaRank model."""

from __future__ import annotations

import warnings
from pathlib import Path
from typing import Any

import numpy as np
import pandas as pd

from .artifact import ArtifactSpec
from .preprocessing import model_matrix

try:
    import lightgbm as lgb
    HAVE_LGBM = True
except ImportError:  # pragma: no cover
    lgb = None
    HAVE_LGBM = False


SEED = 42
DEFAULT_PARAMS: dict[str, Any] = {
    "objective": "lambdarank",
    "metric": "ndcg",
    "boosting_type": "gbdt",
    "subsample": 0.9,
    "subsample_freq": 1,
    "colsample_bytree": 0.9,
    "reg_lambda": 1.0,
    "random_state": SEED,
    "n_jobs": 4,
    "verbose": -1,
}


def _require_lightgbm() -> None:
    if not HAVE_LGBM:
        raise RuntimeError("lightgbm is required to train or load an LTR model")


def fit_ranker(X: pd.DataFrame, labels: np.ndarray, groups: np.ndarray,
               params: dict[str, Any] | None = None):
    """Low-level fit entry point shared with benchmark cross-validation."""
    _require_lightgbm()
    model = lgb.LGBMRanker(**{**DEFAULT_PARAMS, **(params or {})})
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        model.fit(X, labels, group=groups)
    return model


class LambdaRankModel:
    """A LightGBM booster coupled to its ordered feature contract."""

    def __init__(self, booster, spec: ArtifactSpec):
        self.booster = booster
        self.spec = spec

    @classmethod
    def fit(cls, frame: pd.DataFrame, feature_columns: list[str], *,
            label_column: str = "is_positive",
            group_column: str = "instance_id",
            normalize: bool = True,
            candidate_policy: str = "all_no_init",
            params: dict[str, Any] | None = None) -> "LambdaRankModel":
        """Fit one final model using parameters selected in ``bench.ltr.cv``."""
        required = [group_column, label_column, *feature_columns]
        missing = [c for c in required if c not in frame.columns]
        if missing:
            raise ValueError(f"training data is missing columns: {missing}")
        if frame.empty:
            raise ValueError("training data is empty")

        ordered = frame.sort_values(group_column, kind="stable").reset_index(drop=True)
        normalization = "per_query_minmax" if normalize else "none"
        X = model_matrix(ordered, feature_columns, group_column, normalization)
        labels = ordered[label_column].to_numpy(dtype=int)
        groups = ordered.groupby(group_column, sort=False).size().to_numpy()
        chosen = dict(params or {})
        ranker = fit_ranker(X, labels, groups, chosen)
        spec = ArtifactSpec(
            feature_columns=tuple(feature_columns),
            group_column=group_column,
            normalization=normalization,
            candidate_policy=candidate_policy,
            training_parameters={**DEFAULT_PARAMS, **chosen},
        )
        return cls(ranker.booster_, spec)

    def predict(self, frame: pd.DataFrame) -> np.ndarray:
        self.spec.require_columns(list(frame.columns))
        X = model_matrix(frame, list(self.spec.feature_columns),
                         self.spec.group_column, self.spec.normalization)
        return np.asarray(self.booster.predict(X), dtype=float)

    def feature_importance(self) -> pd.Series:
        values = self.booster.feature_importance("gain")
        return pd.Series(values, index=self.spec.feature_columns, name="gain")

    def save(self, directory: Path | str) -> None:
        target = Path(directory)
        target.mkdir(parents=True, exist_ok=True)
        self.booster.save_model(str(target / self.spec.model_file))
        self.spec.save(target)

    @classmethod
    def load(cls, directory: Path | str) -> "LambdaRankModel":
        _require_lightgbm()
        target = Path(directory)
        spec = ArtifactSpec.load(target)
        booster = lgb.Booster(model_file=str(target / spec.model_file))
        if booster.num_feature() != len(spec.feature_columns):
            raise ValueError(
                "model feature count does not match artifact metadata: "
                f"{booster.num_feature()} != {len(spec.feature_columns)}")
        return cls(booster, spec)
