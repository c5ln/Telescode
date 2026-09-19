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
