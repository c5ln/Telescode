"""Train and save one final LambdaRank artifact from a feature CSV.

Example:
    bench/.venv/bin/python -m ltr.train \
      --features bench/data/features.csv \
      --columns pagerank,bc,ppr,bm25,bm25_rank \
      --output ltr/models/local
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import pandas as pd

from .model import LambdaRankModel


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--features", required=True, help="training feature CSV")
    parser.add_argument("--columns", required=True,
                        help="ordered comma-separated model feature columns")
    parser.add_argument("--output", required=True, help="artifact directory")
    parser.add_argument("--group-column", default="instance_id")
    parser.add_argument("--label-column", default="is_positive")
    parser.add_argument("--candidate-policy", default="all_no_init")
    parser.add_argument("--no-normalize", action="store_true")
    parser.add_argument("--params-json", default=None,
                        help="optional JSON object/file with selected LightGBM params")
    args = parser.parse_args(argv)

    columns = [c.strip() for c in args.columns.split(",") if c.strip()]
    if not columns:
        raise SystemExit("--columns must contain at least one feature")
    params = None
    if args.params_json:
        raw = args.params_json
        if not raw.lstrip().startswith("{"):
            raw = Path(raw).read_text()
        params = json.loads(raw)

    frame = pd.read_csv(args.features)
    model = LambdaRankModel.fit(
        frame, columns, label_column=args.label_column,
        group_column=args.group_column, normalize=not args.no_normalize,
        candidate_policy=args.candidate_policy, params=params)
    model.save(args.output)
    print(f"saved {args.output}: {len(columns)} features, {len(frame)} rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
