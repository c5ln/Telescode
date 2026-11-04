"""The onboarding path must run the native model, preserving baseline features."""

import json
import subprocess
import sys

import pytest

from ltr.tests.test_features import project, HEADLESS, PPR, ARTIFACT
from ltr.features import build_features
from ltr.model import LambdaRankModel
from ltr.rank import rank_files


@pytest.mark.parametrize("query", [None, "load_dataset invalid dataset contents"])
def test_tour_ranking_matches_reference_and_preserves_database(project, query):
    from ltr.tour_rank import rank_repository, DEFAULT_ONBOARDING_QUERY
    repo, db, _ = project
    before = db.read_bytes()
    kwargs = {} if query is None else {"query": query}
    actual = rank_repository(repo, db, headless_bin=HEADLESS, ppr_bin=PPR,
                             model_dir=ARTIFACT, **kwargs)
    expected_query = DEFAULT_ONBOARDING_QUERY if query is None else query
    frame = build_features(repo, db, expected_query, query_id="tour",
                           headless_bin=HEADLESS, ppr_bin=PPR)
    expected = rank_files(LambdaRankModel.load(ARTIFACT), frame)
    assert actual["query"] == expected_query
    assert actual["queryId"] == "tour"
    assert actual["model"] == "lambdarank-v1"
    assert [f["fileId"] for f in actual["files"]] == expected.file_id.tolist()
    assert [f["rank"] for f in actual["files"]] == expected["rank"].tolist()
    assert [f["score"] for f in actual["files"]] == pytest.approx(expected.score.tolist(), abs=1e-12)
    assert db.read_bytes() == before


@pytest.mark.parametrize("query", ["", "  ", "qxzzunseenidentifier"])
def test_tour_ranking_rejects_unusable_query_without_fallback(project, query):
    from ltr.tour_rank import rank_repository
    repo, db, _ = project
    with pytest.raises(ValueError, match="query|BM25"):
        rank_repository(repo, db, query=query, headless_bin=HEADLESS,
                        ppr_bin=PPR, model_dir=ARTIFACT)


def test_native_failure_is_not_reported_as_a_successful_tour_ranking(project, tmp_path):
    from ltr.tour_rank import rank_repository
    with pytest.raises(ValueError, match="rank.*failed"):
        rank_repository(project[0], project[1], query="load_dataset",
                        headless_bin=HEADLESS, ppr_bin=PPR, model_dir=tmp_path)


def test_tour_rank_cli_outputs_only_json_with_native_results(project):
    repo, db, _ = project
    result = subprocess.run([
        sys.executable, "-m", "ltr.tour_rank",
        "--repo", str(repo), "--db", str(db), "--headless-bin", str(HEADLESS),
        "--ppr-bin", str(PPR), "--model", str(ARTIFACT),
    ], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    actual = json.loads(result.stdout)
    assert len(actual["files"]) == 3
    assert all(f["fileId"] != "pkg/__init__.py" for f in actual["files"])


@pytest.mark.parametrize("query", ["load_dataset invalid dataset contents", "unrelated"])
def test_tour_rank_cli_forwards_user_goal_to_the_existing_query_ranker(project, query):
    from ltr.tour_rank import rank_repository
    repo, db, _ = project
    before = db.read_bytes()
    result = subprocess.run([
        sys.executable, "-m", "ltr.tour_rank", "--repo", str(repo), "--db", str(db),
        "--headless-bin", str(HEADLESS), "--ppr-bin", str(PPR), "--model", str(ARTIFACT),
        "--query", query,
    ], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    actual = json.loads(result.stdout)
    expected = rank_repository(repo, db, headless_bin=HEADLESS, ppr_bin=PPR, model_dir=ARTIFACT, query=query)
    assert actual == expected
    assert actual["query"] == query
    assert db.read_bytes() == before
