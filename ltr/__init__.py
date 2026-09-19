"""Production-facing Learning-to-Rank model package.

Benchmark orchestration stays in :mod:`bench.ltr`; this package owns the
train/save/load/predict contract consumed by production runtimes.
"""

from .artifact import ArtifactSpec
from .model import LambdaRankModel

__all__ = ["ArtifactSpec", "LambdaRankModel"]
