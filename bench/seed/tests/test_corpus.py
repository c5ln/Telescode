"""corpus 구성 테스트 — 실제 git repo를 임시로 만들어 검증한다.

특히 확인하는 것: **base_commit 시점 트리만** 읽는가.
이후 커밋의 내용이 섞이면 gold 파일에 정답 코드가 들어가 실험이 무효가 된다.
"""
import subprocess

import pytest

from bench.seed.corpus import build_corpus, list_source_blobs


@pytest.fixture
def repo(tmp_path):
    d = tmp_path / "r"
    d.mkdir()
    def git(*a):
        subprocess.run(["git", "-C", str(d), *a], check=True, capture_output=True)
    git("init", "-q")
    git("config", "user.email", "t@t"); git("config", "user.name", "t")

    (d / "pkg").mkdir()
    (d / "pkg" / "alpha.py").write_text("def get_records():\n    return CAPLOG_STASH\n")
    (d / "README.md").write_text("not source\n")
    git("add", "-A"); git("commit", "-qm", "c1")
    base = subprocess.run(["git", "-C", str(d), "rev-parse", "HEAD"],
                          capture_output=True, check=True).stdout.decode().strip()

    # base 이후 커밋 — corpus에 절대 새어 들어오면 안 된다
    (d / "pkg" / "alpha.py").write_text("def get_records():\n    return FIXED_LATER\n")
    (d / "pkg" / "beta.py").write_text("x = 1\n")
    git("add", "-A"); git("commit", "-qm", "c2")
    return d, base


def test_only_source_suffixes_listed(repo):
    d, base = repo
    paths = [p for p, _ in list_source_blobs(d, base)]
    assert paths == ["pkg/alpha.py"]
    assert "README.md" not in paths


def test_reads_base_commit_tree_not_head(repo):
    d, base = repo
    c = build_corpus(d, "i1", base)
    assert c.doc_ids == ["pkg/alpha.py"], "이후 커밋의 beta.py가 들어오면 안 된다"
    toks = c.docs[0]
    assert "caplog_stash" in toks
    assert "fixed_later" not in toks, "패치 이후 내용이 새면 실험 전체가 무효다"


def test_path_tokens_included_and_weighted(repo):
    d, base = repo
    c1 = build_corpus(d, "i1", base, path_weight=1)
    c3 = build_corpus(d, "i1", base, path_weight=3)
    assert c1.docs[0].count("alpha") == c3.docs[0].count("alpha") - 2
    assert c1.docs[0].count("pkg") == 1


def test_candidate_filter_restricts_corpus(repo):
    d, base = repo
    c = build_corpus(d, "i1", base, candidate_file_ids={"nope.py"})
    assert c.doc_ids == []


def test_batch_read_handles_many_blobs(repo):
    """cat-file --batch 파서가 여러 blob 경계를 정확히 자르는지."""
    d, base = repo
    subprocess.run(["git", "-C", str(d), "checkout", "-q", base, "-b", "w"], check=True,
                   capture_output=True)
    for i in range(30):
        (d / f"m{i:02d}.py").write_text(f"VALUE_{i} = {i}\n" * (i + 1))
    subprocess.run(["git", "-C", str(d), "add", "-A"], check=True, capture_output=True)
    subprocess.run(["git", "-C", str(d), "commit", "-qm", "many"], check=True,
                   capture_output=True)
    head = subprocess.run(["git", "-C", str(d), "rev-parse", "HEAD"],
                          capture_output=True, check=True).stdout.decode().strip()
    c = build_corpus(d, "i2", head)
    assert len(c.doc_ids) == 31 and not c.skipped
    for i in range(30):
        doc = c.docs[c.doc_ids.index(f"m{i:02d}.py")]
        assert f"value_{i}" in doc, "blob 경계가 어긋나면 다른 파일 내용이 섞인다"
