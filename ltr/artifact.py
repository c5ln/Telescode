"""Versioned metadata for a portable LambdaRank artifact."""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any


ARTIFACT_VERSION = 1
MODEL_FILENAME = "model.txt"
SPEC_FILENAME = "artifact.json"


@dataclass(frozen=True)
class ArtifactSpec:
    """Everything a runtime needs in addition to the LightGBM model file.

    ``feature_columns`` is ordered: changing its order changes model input.
    The JSON sidecar is deliberately language-neutral so a C++ loader can
    validate the same contract before calling LightGBM.
    """

    feature_columns: tuple[str, ...]
    group_column: str = "instance_id"
    normalization: str = "per_query_minmax"
    candidate_policy: str = "all_no_init"
    model_type: str = "lightgbm_lambdarank"
    model_file: str = MODEL_FILENAME
    artifact_version: int = ARTIFACT_VERSION
    training_parameters: dict[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        if self.artifact_version != ARTIFACT_VERSION:
            raise ValueError(
                f"unsupported artifact version: {self.artifact_version}")
        if not self.feature_columns:
            raise ValueError("feature_columns must not be empty")
        if len(set(self.feature_columns)) != len(self.feature_columns):
            raise ValueError("feature_columns contains duplicates")
        if self.normalization not in {"none", "per_query_minmax"}:
            raise ValueError(f"unsupported normalization: {self.normalization}")

    def save(self, directory: Path) -> Path:
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / SPEC_FILENAME
        payload = asdict(self)
        payload["feature_columns"] = list(self.feature_columns)
        path.write_text(
            json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8")
        return path

    @classmethod
    def load(cls, directory: Path) -> "ArtifactSpec":
        path = directory / SPEC_FILENAME
        payload = json.loads(path.read_text(encoding="utf-8"))
        payload["feature_columns"] = tuple(payload["feature_columns"])
        return cls(**payload)

    def require_columns(self, available: list[str]) -> None:
        missing = [c for c in self.feature_columns if c not in available]
        if missing:
            raise ValueError(f"model input is missing features: {missing}")
