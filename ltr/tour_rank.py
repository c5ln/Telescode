"""Rank a live repository for the existing onboarding tour workflow.

Python generates raw features; TelescodeHeadless performs model inference.
The optional query parameter is internal for now: desktop tours use the default.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
import tempfile

from .features import ARTIFACT, _run, build_features


DEFAULT_ONBOARDING_QUERY = (
    "Understand the application architecture, main entry points, core modules, "
    "configuration, dependencies, and how to load, process and return data."
)


def rank_repository(repo: Path, db: Path, *, headless_bin: Path, ppr_bin: Path,
                    model_dir: Path = ARTIFACT,
                    query: str = DEFAULT_ONBOARDING_QUERY) -> dict:
    """Return model scores without modifying source, baseline ranks or metrics."""
    frame = build_features(repo, db, query, query_id="tour",
                           headless_bin=headless_bin, ppr_bin=ppr_bin)
    with tempfile.TemporaryDirectory(prefix="telescode-tour-rank-") as temporary:
        features = Path(temporary) / "features.csv"
        ranked = Path(temporary) / "ranked.csv"
        frame.to_csv(features, index=False)
        _run(headless_bin, ["rank", "--model", str(model_dir),
                           "--features", str(features), "--output", str(ranked)], "ranking")
        with ranked.open(encoding="utf-8", newline="") as stream:
            files = [{"fileId": row["file_id"], "score": float(row["score"]),
                      "rank": int(row["rank"])} for row in csv.DictReader(stream)]
    if not files or any(not math.isfinite(row["score"]) for row in files):
        raise ValueError("ranking backend returned empty or non-finite scores")
    return {"model": Path(model_dir).name, "query": query, "queryId": "tour", "files": files}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--db", required=True, type=Path)
    parser.add_argument("--headless-bin", required=True, type=Path)
    parser.add_argument("--ppr-bin", required=True, type=Path)
    parser.add_argument("--model", type=Path, default=ARTIFACT)
    args = parser.parse_args(argv)
    try:
        result = rank_repository(args.repo, args.db, headless_bin=args.headless_bin,
                                 ppr_bin=args.ppr_bin, model_dir=args.model)
    except (ValueError, KeyError, OSError) as exc:
        parser.exit(1, f"tour ranking failed: {exc}\n")
    print(json.dumps(result, ensure_ascii=False, allow_nan=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
