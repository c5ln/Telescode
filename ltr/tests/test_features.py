"""Live feature generation must match the training pipeline before ranking."""

from pathlib import Path
import sqlite3
import subprocess

import numpy as np
import pandas as pd
import pytest

from bench.features.code_lexical import (
    def_match_scores, defined_names, literal_segments, neighbor_aggregates,
    normalize_text, query_identifiers, str_match_scores,
)
from bench.features.extract import _file_level_edges, extract
from bench.seed.bm25 import BM25
from bench.seed.run_seed import rank_pessimistic
from bench.seed.textproc import make_query
from bench.seed.tokenizer import tokenize, tokenize_path
from ltr.artifact import ArtifactSpec
from ltr.model import LambdaRankModel
from ltr.rank import rank_files


ROOT = Path(__file__).resolve().parents[2]
HEADLESS = ROOT / "build/TelescodeHeadless"
PPR = ROOT / "build/TelescodePPR"
ARTIFACT = ROOT / "ltr/models/lambdarank-v1"
QUERY = ('open_dataset reports invalid dataset contents\n'
         'Traceback (most recent call last):\n'
         '  File "pkg/beta.py", line 3, in load_dataset\n'
         '    return open_dataset()\n'
         'ValueError: invalid dataset contents\n')


@pytest.fixture
def project(tmp_path):
    repo = tmp_path / "repo"
    (repo / "pkg").mkdir(parents=True)
    sources = {
        "pkg/__init__.py": "from .alpha import open_dataset\n",
        "pkg/alpha.py": ('def open_dataset():\n'
                         '    raise ValueError("invalid dataset contents")\n'),
        "pkg/beta.py": ('from .alpha import open_dataset\n'
                        'def load_dataset():\n    return open_dataset()\n'),
        "isolated.py": "def unrelated():\n    return 17\n",
    }
    for name, text in sources.items():
        (repo / name).write_text(text)
    db = tmp_path / "project.db"
    for args in [["scan", str(repo), str(db)], ["algo", str(db)]]:
        subprocess.run([str(HEADLESS), *args], check=True, capture_output=True)
    return repo, db, sources


def build(project, query=QUERY):
    from ltr.features import build_features
    repo, db, _ = project
    return build_features(repo, db, query, query_id="NA",
                          headless_bin=HEADLESS, ppr_bin=PPR)


def reference(project, tmp_path):
    repo, db, sources = project
    f = extract(db, "NA", []).sort_values("file_id").reset_index(drop=True)
    ids = f.file_id.tolist()
    docs = [tokenize_path(fid) + tokenize(sources[fid]) for fid in ids]
    query = make_query(QUERY, "no_paths")
    scores = BM25.build(ids, docs).score_query(tokenize(query))
    f["bm25"] = scores
    f["bm25_rank"] = rank_pessimistic(scores)
    qids = query_identifiers(query)
    f["def_match"] = def_match_scores([defined_names(sources[f]) for f in ids], qids)
    f["str_match"] = str_match_scores([literal_segments(sources[f]) for f in ids],
                                       normalize_text(query))
    with sqlite3.connect(db) as conn:
        edges = _file_level_edges(conn, set(ids))
    mx, mean = neighbor_aggregates(ids, dict(zip(ids, scores)), edges)
    f["nbr_bm25_max"] = f.file_id.map(mx)
    f["nbr_bm25_mean"] = f.file_id.map(mean)
    best = sorted(zip(ids, scores), key=lambda x: (-x[1], x[0]))[0]
    seed, ppr = tmp_path / "reference_seed.csv", tmp_path / "reference_ppr.csv"
    pd.DataFrame([best], columns=["file_id", "weight"]).to_csv(seed, index=False)
    subprocess.run([str(PPR), str(db), str(seed), str(ppr)], check=True, capture_output=True)
    values = pd.read_csv(ppr).set_index("file_id").ppr
    f["ppr"] = f.file_id.map(values).fillna(0.)
    columns = ["instance_id", "file_id", *ArtifactSpec.load(ARTIFACT).feature_columns]
    return f[columns]


def test_all_19_raw_features_match_benchmark_and_cpp_ppr(project, tmp_path):
    expected = reference(project, tmp_path)
    actual = build(project)
    pd.testing.assert_frame_equal(actual, expected, check_dtype=False, check_exact=True)
    assert len(actual) == 4  # initializers affect corpus/IDF and are filtered later
    assert actual.def_match.max() > 0
    assert actual.str_match.max() > 0
    isolated = actual.set_index("file_id").loc["isolated.py"]
    assert isolated["nbr_bm25_max"] == isolated["nbr_bm25_mean"] == 0
    assert np.isfinite(actual.iloc[:, 2:].to_numpy(dtype=float)).all()
    model = LambdaRankModel.load(ARTIFACT)
    assert "pkg/__init__.py" not in set(rank_files(model, actual).file_id)


def test_query_preprocessing_drops_paths_and_traceback_frames(project):
    cleaned = make_query(QUERY, "no_paths")
    pd.testing.assert_frame_equal(build(project), build(project, cleaned), check_exact=True)


def test_untracked_source_outside_scan_does_not_change_corpus(project):
    before = build(project)
    (project[0] / "unscanned.py").write_text("open_dataset " * 500)
    pd.testing.assert_frame_equal(build(project), before, check_exact=True)


@pytest.mark.parametrize("query", ["", "  \n", "qxzzunseenidentifier"])
def test_empty_or_unmatched_query_fails_without_plain_pagerank_fallback(project, query):
    with pytest.raises(ValueError, match="query|BM25"):
        build(project, query)


def test_missing_source_fails_with_rescan_guidance(project):
    (project[0] / "pkg/alpha.py").unlink()
    with pytest.raises(ValueError, match="source|rescan"):
        build(project)


def test_scan_without_algo_fails_instead_of_emitting_missing_static_scores(project):
    repo, db, _ = project
    subprocess.run([str(HEADLESS), "scan", str(repo), str(db)], check=True, capture_output=True)
    with pytest.raises(ValueError, match="algo"):
        build(project)


def test_wrong_db_version_is_refused_without_modifying_it(project):
    with sqlite3.connect(project[1]) as conn:
        conn.execute("PRAGMA user_version = 999")
    before = project[1].read_bytes()
    with pytest.raises(ValueError, match="rescan"):
        build(project)
    assert project[1].read_bytes() == before


def test_missing_db_is_not_created(project):
    missing = project[1].with_name("missing.db")
    with pytest.raises(ValueError):
        build((project[0], missing, project[2]))
    assert not missing.exists()


def test_ppr_backend_failure_is_not_reported_as_success(project):
    from ltr.features import build_features
    with pytest.raises(ValueError, match="PPR"):
        build_features(project[0], project[1], QUERY, query_id="NA",
                       headless_bin=HEADLESS, ppr_bin=Path("/bin/false"))


def test_cli_emits_rankable_features_without_rewriting_db(project, tmp_path):
    from ltr.features import main
    repo, db, _ = project
    before = db.read_bytes()
    query, output = tmp_path / "query.txt", tmp_path / "features.csv"
    query.write_text(QUERY)
    args = ["--repo", str(repo), "--db", str(db), "--query-file", str(query),
            "--query-id", "NA", "--headless-bin", str(HEADLESS),
            "--ppr-bin", str(PPR), "--output", str(output)]
    assert main(args) == 0
    actual = pd.read_csv(output, keep_default_na=False)
    pd.testing.assert_frame_equal(actual, build(project), check_dtype=False)
    assert db.read_bytes() == before
    for protected in [db, query, repo / "pkg/alpha.py"]:
        with pytest.raises(SystemExit):
            main([*args[:-1], str(protected)])
    assert db.read_bytes() == before
