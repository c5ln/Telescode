# C++ LambdaRank inference contract (tests first)

These tests were written and observed failing before native inference was
implemented. `RankingTests` now exercises `ranking/LambdaRankModel.h`, while
the Python acceptance tests exercise the real `TelescodeHeadless rank` command.

## Native service interface required by the tests

All types are in namespace `TS`:

```cpp
struct LtrFeatureRow {
    std::string query_id;
    std::string file_id;
    std::vector<double> values;
};
struct LtrFeatureTable {
    std::vector<std::string> columns;
    std::vector<LtrFeatureRow> rows;
};
struct LtrRankedFile {
    std::string query_id;
    std::string file_id;
    double score;
    int rank;
};
class LambdaRankModel {
public:
    static LambdaRankModel Load(const std::string& artifact_directory);
    const std::vector<std::string>& featureColumns() const;
    LtrFeatureTable prepare(const LtrFeatureTable&) const;
    std::vector<LtrRankedFile> rank(const LtrFeatureTable&) const;
};
```

This is the public contract sketch; the production header also declares move
ownership and exposes the metadata's group column. Artifact/load failures throw
`std::runtime_error`; invalid feature tables throw `std::invalid_argument`.
`prepare` excludes initializers, preserves surviving input row order, selects
columns in metadata order, and applies per-query normalization. Extra input
columns are ignored. `rank` returns query ID ascending, score descending, file
ID ascending, with 1-based ranks reset for each query.

The CLI contract is:

```sh
build/TelescodeHeadless rank --model ltr/models/lambdarank-v1 --features input.csv --output ranked.csv
```

Its output is `instance_id,file_id,score,rank`, matching `python -m ltr.rank`.
Errors must leave an existing output intact and never overwrite input/model
files. The fixture includes `NA`, `0001`, Unicode, commas and quoted IDs.

## Oracle data

- `input.csv`: ten raw rows, interleaved queries, reversed feature order and an
  unused column; includes three initializers with extreme values.
- `prepared.csv`: seven selected rows, 19 normalized columns in model order,
  constants and missing values included.
- `ranked.csv`: Python LightGBM scores and exact file ordering, including ties.
- `none_prepared.csv` and `none_ranked.csv`: all seven candidate rows and all
  19 raw columns, plus reference scores/ranks with normalization set to `none`.
  Native tests compare column names, row count/IDs, every row width and value,
  and scores/ranks. An empty result or matching only the first row cannot pass.
- `boundary_input.csv`, `boundary_prepared.csv`, `boundary_ranked.csv` and
  `boundary.json`: a real split threshold and its immediately adjacent doubles,
  plus 0/1 anchors so normalization preserves those values. Python checks that
  deliberately casting inference inputs to float32 fails the score comparison.
  It also checks that applying min-max to the `none` fixture fails comparison.
- `manifest.json`: SHA-256 of the released model, metadata and data files.

Regenerate only when deliberately changing the reference contract/model:

```sh
bench/.venv/bin/python -m ltr.tests.generate_cpp_rank_fixture
```

Prepared finite matrix values use `EXPECT_DOUBLE_EQ`; NaN stays NaN. Scores use
absolute/relative tolerance 1e-12; ranks and IDs must agree exactly. The fixture
is also checked against current Python inference so stale oracle files fail.
It is a mechanics/parity fixture, not an evaluation of ranking quality.

Current test state: 27 native unit-test cases and all 17 Python oracle/CLI
cases pass. These include the 13 native CLI cases that previously failed,
three oracle/sensitivity checks, and a regression check proving the Python
CSV reader also preserves split-boundary precision. Native inference calls
LightGBM directly; there is no Python fallback for the native command.

## Run

```sh
cmake -S . -B build -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build build --target RankingTests -j4
bench/.venv/bin/python -m pytest ltr/tests/test_cpp_rank.py -q
```

For a headless binary in another build directory, set `TELESCODE_HEADLESS_BIN`
when running pytest. The native command is never replaced with Python or skipped.
