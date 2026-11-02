"""Generate the released 19-feature model's raw inputs from a live repository.

The database must already contain scan/update and algo results for this source
tree. Python owns query text features; the existing C++ commands own graph
analysis and PPR. No benchmark dataset, labels, Git checkout, or API is needed.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import tempfile

import numpy as np
import pandas as pd

from .artifact import ArtifactSpec
from .retrieval.bm25 import BM25, rank_pessimistic
from .retrieval.lexical import (
    defined_names, def_match_scores, literal_segments, neighbor_aggregates,
    normalize_text, query_identifiers, str_match_scores,
)
from .retrieval.textproc import make_query, NO_PATHS
from .retrieval.tokenizer import tokenize, tokenize_path


ARTIFACT = Path(__file__).resolve().parent / "models/lambdarank-v1"
MAX_SOURCE_BYTES = 400_000  # Same cap as the training corpus and lexical features.


def _run(binary: Path, args: list[str], name: str) -> str:
    try:
        proc = subprocess.run([str(binary), *args], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise ValueError(f"{name} backend failed: {exc}") from exc
    if proc.returncode:
        raise ValueError(f"{name} backend failed ({proc.returncode}): {proc.stderr.strip()}")
    return proc.stdout


def _ppr(db: Path, binary: Path, file_ids: list[str], scores: list[float]) -> dict[str, float]:
    # v1 was trained with BM25 top-1, power=1. Do not use the teleport helper's
    # top-20 default, or silently substitute uniform PageRank for no matches.
    positive = sorted(((fid, score) for fid, score in zip(file_ids, scores) if score > 0),
                      key=lambda item: (-item[1], item[0]))
    if not positive:
        raise ValueError("query has no positive BM25 matches; PPR is undefined for this input")
    with tempfile.TemporaryDirectory(prefix="telescode-ppr-") as tmp:
        seed, output = Path(tmp) / "seed.csv", Path(tmp) / "ppr.csv"
        pd.DataFrame(positive[:1], columns=["file_id", "weight"]).to_csv(seed, index=False)
        _run(binary, [str(db), str(seed), str(output)], "PPR")
        if not output.is_file():
            raise ValueError("PPR backend did not produce its output")
        values = pd.read_csv(output, converters={"file_id": str})
    if not {"file_id", "ppr"} <= set(values.columns) or values.empty:
        raise ValueError("PPR backend returned missing rows or columns")
    weights = values["ppr"].to_numpy(dtype=float)
    if (values.file_id.duplicated().any() or
            not set(values.file_id) <= set(file_ids) or
            not np.isfinite(weights).all() or (weights < 0).any() or weights.sum() <= 0):
        raise ValueError("PPR backend returned invalid file scores")
    return dict(zip(values.file_id, weights))


def build_features(repo: Path, db: Path, query: str, *, query_id: str,
                   headless_bin: Path, ppr_bin: Path) -> pd.DataFrame:
    """Return instance_id, file_id, and 19 raw columns in artifact order.

    The full scanned file set participates in BM25, IDF, neighbor features and
    PPR. Candidate exclusion and per-query normalization belong to rank_files.
    Equal BM25 scores get the worst tied rank, matching training.
    """
    if not isinstance(query_id, str) or not query_id.strip():
        raise ValueError("query_id must be a non-empty string")
    if not isinstance(query, str) or not query.strip():
        raise ValueError("query must be non-empty")
    query = make_query(query, NO_PATHS)
    if not tokenize(query):
        raise ValueError("query has no searchable tokens after preprocessing")
    repo, db = Path(repo).resolve(), Path(db).resolve()
    if not repo.is_dir():
        raise ValueError(f"repository directory does not exist: {repo}")
    if not db.is_file():
        raise ValueError(f"database does not exist: {db}; run scan and algo first")

    # This command checks the database format before reading, so Python has no
    # independent schema-version constant or legacy graph interpretation.
    snapshot = json.loads(_run(Path(headless_bin), ["analyze", str(db)], "analysis"))
    if snapshot.get("schemaVersion") != 1:
        raise ValueError("unsupported analysis JSON schema")
    files = sorted(snapshot["files"], key=lambda row: row["fileId"])
    if not files:
        raise ValueError("database has no scanned files; run scan and algo first")
    file_ids = [row["fileId"] for row in files]
    sequence = {row["fileId"]: row for row in snapshot["readingSequence"]
                if row["entityType"] == "file"}
    if set(sequence) != set(file_ids):
        raise ValueError("missing file reading scores; run algo after scan/update")
    edges = {(edge["source"], edge["target"]) for edge in snapshot["fileGraph"]["edges"]
             if edge["source"] in sequence and edge["target"] in sequence
             and edge["source"] != edge["target"]}
    inbound = dict.fromkeys(file_ids, 0)
    outbound = dict.fromkeys(file_ids, 0)
    for source, target in edges:
        outbound[source] += 1
        inbound[target] += 1

    sources, rows = [], []
    for file in files:
        fid = file["fileId"]
        source = (repo / fid).resolve()
        if Path(fid).is_absolute() or not source.is_relative_to(repo):
            raise ValueError(f"source is outside repository: {fid}")
        if file["language"] != "python" or not fid.endswith(".py"):
            raise ValueError(f"unsupported source language: {fid}")
        try:
            with source.open("rb") as stream:
                sources.append(stream.read(MAX_SOURCE_BYTES).decode("utf-8", errors="replace"))
        except OSError as exc:
            raise ValueError(f"cannot read source {fid}; update/rescan the database: {exc}") from exc
        metric, seq = file["metrics"], sequence[fid]
        rows.append({
            "instance_id": query_id, "file_id": fid,
            "pagerank": seq["scores"]["pagerank"], "bc": seq["scores"]["betweenness"],
            "combined": seq["scores"]["combined"], "file_rank": seq["fileRank"],
            "in_deg": inbound[fid], "out_deg": outbound[fid],
            "complexity": metric["complexityScore"],
            "max_cc": metric["maxCyclomaticComplexity"],
            "avg_cc": metric["avgCyclomaticComplexity"],
            "max_depth": metric["maxBlockDepth"], "avg_depth": metric["avgBlockDepth"],
            "logical_loc": metric["logicalLoc"],
        })
    frame = pd.DataFrame(rows)
    docs = [tokenize_path(fid) + tokenize(source) for fid, source in zip(file_ids, sources)]
    scores = BM25.build(file_ids, docs).score_query(tokenize(query))
    frame["bm25"] = scores
    frame["bm25_rank"] = rank_pessimistic(scores)
    values = _ppr(db, Path(ppr_bin), file_ids, scores)
    frame["ppr"] = frame.file_id.map(values).fillna(0.)
    qids = query_identifiers(query)
    frame["def_match"] = def_match_scores([defined_names(s) for s in sources], qids)
    frame["str_match"] = str_match_scores([literal_segments(s) for s in sources],
                                           normalize_text(query))
    mx, mean = neighbor_aggregates(file_ids, dict(zip(file_ids, scores)), edges)
    frame["nbr_bm25_max"] = frame.file_id.map(mx)
    frame["nbr_bm25_mean"] = frame.file_id.map(mean)
    spec = ArtifactSpec.load(ARTIFACT)
    spec.require_columns(list(frame.columns))
    return frame[[spec.group_column, "file_id", *spec.feature_columns]]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--db", required=True, type=Path)
    parser.add_argument("--query-file", required=True, type=Path)
    parser.add_argument("--query-id", default="query")
    parser.add_argument("--headless-bin", required=True, type=Path)
    parser.add_argument("--ppr-bin", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    # A feature export must not replace the database, source tree, query, model
    # artifacts, or the executables it just read.
    protected = [args.db, args.query_file, args.headless_bin, args.ppr_bin,
                 ARTIFACT / "artifact.json", ARTIFACT / "model.txt"]
    if args.output.resolve().is_relative_to(args.repo.resolve()):
        parser.error("--output must be outside the source repository")
    for path in protected:
        if (args.output.resolve() == path.resolve() or
                (args.output.exists() and path.exists() and args.output.samefile(path))):
            parser.error("--output would overwrite an input")
    try:
        frame = build_features(args.repo, args.db, args.query_file.read_text(encoding="utf-8"),
                               query_id=args.query_id, headless_bin=args.headless_bin,
                               ppr_bin=args.ppr_bin)
        frame.to_csv(args.output, index=False)
    except (ValueError, KeyError, OSError) as exc:
        parser.exit(1, f"feature generation failed: {exc}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
