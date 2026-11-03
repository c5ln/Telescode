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

The native ranker loads the text model through the LightGBM C API.
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

This command consumes features and serves as the native implementation's
reference. The staged work is tracked in
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

## Native inference

The headless binary now ranks an existing raw feature CSV directly:

```sh
cmake -S . -B build -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build build --target TelescodeHeadless RankingTests -j4
build/TelescodeHeadless rank --model ltr/models/lambdarank-v1 --features /tmp/features.csv --output /tmp/ranked_cpp.csv
```

CMake searches the system and the local `.venv` / `bench/.venv` wheel locations
for the LightGBM C library. For another installation, pass
`-DTS_LIGHTGBM_LIBRARY=/absolute/path/to/lib_lightgbm.so` when configuring.
The linked shared library and its normal platform dependencies must remain
available when running the binary. Python is not invoked by native inference;
the current feature-generation command still uses Python.

`TS::LambdaRankModel` owns/frees the booster, validates feature count and names,
and exposes `prepare` and `rank`. It uses float64 inputs, preserves NaN, supports
`per_query_minmax` and `none`, and sorts ties by file ID. The JSON metadata reader
uses the already bundled SQLite JSON functions in memory. The private C ABI
declarations follow the [official LightGBM C API](https://lightgbm.readthedocs.io/en/stable/C-API.html).
CSV IDs retain strings/quoting, and output is committed only after successful
validation, inference and writing. The Python CSV ranker also uses round-trip
float parsing so both commands retain split-boundary precision.

Validation on Linux with LightGBM 4.7.0: 27 native ranking cases, 17 Python/native
oracle and CLI cases, 289 Python tests overall, and 390 C++ tests across seven
targets passed. The pre-existing LayoutTests target remains excluded.

## Onboarding tour integration

The existing MCP/desktop tour workflow now calls `python -m ltr.tour_rank` when
opening a DB. This combines live feature generation with **native C++** ranking
and returns JSON scores. The service applies those file ranks to a tour-specific
analysis snapshot and preserves class/function local ranks and baseline metrics.
It leaves the source DB unchanged, including `file_rank` used as a model feature.

For now, `DEFAULT_ONBOARDING_QUERY` supplies the text features. No user query UI
is required. `rank_repository(..., query=...)` accepts explicit text for a future
query-driven entry point. This connection verifies pipeline behavior and model
parity; it does not establish generic onboarding ranking quality.

The desktop records the source root beside each newly scanned DB; external DBs
can pass `repoPath` to MCP `open_project`. See
[`mcp/README.md`](../mcp/README.md#lambdarank-onboarding-order) for runtime setup,
source metadata, snapshot matching and existing-tour regeneration behavior.
