"""`bench/env.py` 테스트.

`.env` 파싱은 사소해 보이지만 틀리면 **키가 조용히 잘못 실린다** — 따옴표가
값에 섞여 들어가면 API가 401을 내고, 그때 원인을 키 자체에서 찾게 된다.
"""

import os

import pytest

from bench.env import load_dotenv, parse_env, require, write_env


def test_parses_basic_pairs():
    assert parse_env("A=1\nB=two\n") == {"A": "1", "B": "two"}


def test_strips_wrapping_quotes_only():
    """감싼 따옴표는 벗기고, 값 안의 따옴표는 남긴다."""
    got = parse_env("""A="abc"\nB='def'\nC=gh"ij\nD="k'l"\n""")
    assert got == {"A": "abc", "B": "def", "C": 'gh"ij', "D": "k'l"}


def test_ignores_comments_and_blanks():
    assert parse_env("# note\n\n  \nA=1\n") == {"A": "1"}


def test_hash_inside_value_is_not_a_comment():
    """API 키에 #이 들어갈 수 있다. 값을 잘라먹으면 안 된다."""
    assert parse_env("K=sk-or-v1-ab#cd\n") == {"K": "sk-or-v1-ab#cd"}


def test_accepts_export_prefix():
    """`export K=v` 형태를 그대로 붙여넣는 일이 흔하다."""
    assert parse_env("export K=v\n") == {"K": "v"}


def test_value_may_contain_equals():
    """base64 키는 =로 끝나는 경우가 있다."""
    assert parse_env("K=aGVsbG8=\n") == {"K": "aGVsbG8="}


def test_load_does_not_override_existing_env(tmp_path, monkeypatch):
    """쉘에서 export한 값이 파일을 이긴다."""
    p = tmp_path / ".env"
    p.write_text("TELESCODE_T=from_file\n")
    monkeypatch.setenv("TELESCODE_T", "from_shell")

    applied = load_dotenv(p)
    assert applied == {}
    assert os.environ["TELESCODE_T"] == "from_shell"


def test_load_sets_when_absent(tmp_path, monkeypatch):
    p = tmp_path / ".env"
    p.write_text("TELESCODE_T2=from_file\n")
    monkeypatch.delenv("TELESCODE_T2", raising=False)

    assert load_dotenv(p) == {"TELESCODE_T2": "from_file"}
    assert os.environ["TELESCODE_T2"] == "from_file"


def test_override_flag(tmp_path, monkeypatch):
    p = tmp_path / ".env"
    p.write_text("TELESCODE_T3=file\n")
    monkeypatch.setenv("TELESCODE_T3", "shell")

    load_dotenv(p, override=True)
    assert os.environ["TELESCODE_T3"] == "file"


def test_missing_file_is_silent(tmp_path):
    """.env 없는 클론에서 import만으로 죽으면 안 된다."""
    assert load_dotenv(tmp_path / "nope") == {}


def test_require_reports_what_to_do(monkeypatch):
    monkeypatch.delenv("TELESCODE_MISSING", raising=False)
    with pytest.raises(RuntimeError, match="TELESCODE_MISSING"):
        require("TELESCODE_MISSING")


def test_require_treats_empty_as_missing(monkeypatch):
    """`.env`에 `K=` 만 있는 상태(템플릿 그대로)는 '있음'이 아니다."""
    monkeypatch.setenv("TELESCODE_EMPTY", "")
    with pytest.raises(RuntimeError):
        require("TELESCODE_EMPTY")


def test_write_env_preserves_other_keys_and_sets_0600(tmp_path):
    p = tmp_path / ".env"
    p.write_text("KEEP=yes\nREPLACE=old\n")

    write_env({"REPLACE": "new", "ADDED": "x"}, p)

    assert parse_env(p.read_text()) == {"KEEP": "yes", "REPLACE": "new", "ADDED": "x"}
    assert oct(p.stat().st_mode)[-3:] == "600"
