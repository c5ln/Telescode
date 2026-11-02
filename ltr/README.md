# Learning-to-Rank model

This package owns the production-facing LambdaRank lifecycle:

- ordered feature and preprocessing contract;
- final model training;
- LightGBM text-model plus JSON metadata persistence;
- inference with the same preprocessing used during training.

Benchmark-only code (nested CV, ablation, bootstrap evaluation, RRF and
weighted-sum comparisons) remains in `bench/ltr/`.

Each saved artifact is a directory containing:

```text
model.txt       # LightGBM's portable text format
artifact.json   # feature order, normalization, candidate policy, parameters
```

The text model is intended to be loadable later through the LightGBM C API.
The JSON sidecar must be validated first so C++ supplies features in exactly
the training order and applies the same per-query min-max normalization.

The design and experiment history is indexed chronologically in
[`docs/README.md`](docs/README.md).

## Reference inference

Once raw query/file features have been computed, rank them with:

```sh
bench/.venv/bin/python -m ltr.rank --model ltr/models/lambdarank-v1 --features /tmp/features.csv --output /tmp/ranked.csv
```

The CSV needs `instance_id`, `file_id`, and every feature named in the artifact.
Each `(instance_id, file_id)` must be unique. The runtime applies `all_no_init`
before per-query normalization, preserves NaN, and uses the artifact's exact
feature order. A query with no remaining candidates fails explicitly. Scores
are sorted descending, with file IDs ascending for ties and ranks starting at 1.
The output contains `instance_id,file_id,score,rank`.

`LambdaRankModel.predict` remains the lower-level scoring API for already
selected candidates; `ltr.rank.rank_files` applies the candidate policy and
returns ranked file IDs. `fit` likewise expects candidate selection to have
been applied to its training frame. The loader validates both model feature
count and feature names/order against the artifact.

Duplicate CSV headers are rejected before pandas can rename them. Identifier
strings such as `NA` and `0001` are preserved while missing numeric features
remain NaN. Feature names containing whitespace are rejected before training
because LightGBM can rename them and break model/metadata agreement.

This command consumes features; C++ model inference is a subsequent integration
step. The staged work is tracked in
[`docs/10-runtime-integration.md`](docs/10-runtime-integration.md).

## Generate features for a live query

Use a database whose scan/update and algo results match the current source tree.
The extractor reads existing metrics; it does not refresh stale source metrics
or retrain the model. Build both C++ backends first:

```sh
cmake --build build --target TelescodeHeadless TelescodePPR -j4
build/TelescodeHeadless scan /path/to/repository /tmp/project.db
build/TelescodeHeadless algo /tmp/project.db
bench/.venv/bin/python -m ltr.features --repo /path/to/repository --db /tmp/project.db --query-file /tmp/query.txt --query-id query --headless-bin build/TelescodeHeadless --ppr-bin build/TelescodePPR --output /tmp/features.csv
bench/.venv/bin/python -m ltr.rank --model ltr/models/lambdarank-v1 --features /tmp/features.csv --output /tmp/ranked.csv
```

The generated CSV has two ID columns and the artifact's 19 raw feature columns.
It uses the released v1 profile: `no_paths` query processing, BM25 k1=1.2,
b=0.75, linear query term frequency, one copy of path tokens, and BM25 top-1
PPR seed with power=1. Source text is capped at 400,000 bytes per file, matching
training. All scanned Python files, including `__init__.py`, participate in
corpus statistics, worst-tie BM25 ranks, lexical IDF and neighbor features.
Initializer exclusion and min-max normalization happen later in `ltr.rank`.

Missing source files, missing algo scores, incompatible databases, and PPR
failures produce errors. A query with no positive BM25 matches also fails
explicitly instead of emitting uniform PageRank as a query-specific feature.
Output must be outside the source repository. Feature generation uses neither
benchmark datasets/labels nor external APIs. It reads the working tree, so callers
must update the database after editing source; matching existing file paths alone
cannot establish that the stored metrics are fresh.
