"""update_equiv — 손으로 만든 소형 DB 두 개로 비교 결과를 고정하고, 편집 op를 확인한다."""

import math
import os
import sqlite3
import tomllib

import pytest

from bench.tools.update_equiv import (DEFAULT_SCENARIOS, ScenarioError, apply_edit,
                                      compare_dbs, link_tree, update_argv)

SCHEMA = """
CREATE TABLE file (file_id TEXT PRIMARY KEY, complexity_score REAL);
CREATE TABLE import_resolution (file_id TEXT, module_spec TEXT, resolved_file_id TEXT,
                                status TEXT, PRIMARY KEY (file_id, module_spec));
CREATE TABLE ref (ref_id INTEGER PRIMARY KEY, file_id TEXT, source_id TEXT, kind TEXT,
                  name_path TEXT, line INTEGER, resolved_id TEXT, status TEXT,
                  emits_link INTEGER);
CREATE TABLE reading_sequence (entity_id TEXT PRIMARY KEY, entity_type TEXT, file_id TEXT,
                               file_rank INTEGER, local_rank INTEGER, pagerank_score REAL,
                               bc_score REAL, combined_score REAL);
CREATE TABLE reading_sequence_config (config_key TEXT PRIMARY KEY, config_value TEXT);
CREATE VIEW link(source_id, target_id, link_type) AS
  SELECT source_id, resolved_id, kind FROM ref WHERE emits_link = 1
  UNION ALL
  SELECT DISTINCT file_id, resolved_file_id, 'IMPORTS' FROM import_resolution
   WHERE status = 'resolved';
"""

REFS = [  # ref_id는 비교에서 빠진다
    (1, "a.py", "a.py::f", "CALLS", "g", 3, "b.py::g", "resolved", 1),
    (2, "a.py", "a.py::f", "CALLS", "g", 4, "b.py::g", "resolved", 0),
    (3, "a.py", "a.py::f", "CALLS", "len", 5, None, "builtin", 0),
    (4, "b.py", "b.py::g", "CALLS", "h", 2, None, "unresolved", 0),
]
RS = [
    ("a.py", "file", "a.py", 1, None, 0.25, 0.5, 0.375),
    ("a.py::f", "function", "a.py", None, 1, 0.1, 0.2, 0.15),
    ("b.py", "file", "b.py", 2, None, 0.75, 0.5, 0.625),
]


def _make(path, refs=REFS, rs=RS, config=(("alpha", "0.5"), ("last_computed_at", "1"))):
    conn = sqlite3.connect(path)
    conn.executescript(SCHEMA)
    conn.executemany("INSERT INTO file VALUES (?, ?)", [("a.py", 1.0), ("b.py", 2.0)])
    conn.executemany("INSERT INTO import_resolution VALUES (?, ?, ?, ?)", [
        ("a.py", "b", "b.py", "resolved"), ("a.py", "numpy", None, "external")])
    conn.executemany("INSERT INTO ref VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)", refs)
    conn.executemany("INSERT INTO reading_sequence VALUES (?, ?, ?, ?, ?, ?, ?, ?)", rs)
    conn.executemany("INSERT INTO reading_sequence_config VALUES (?, ?)", config)
    conn.commit()
    return conn


@pytest.fixture
def dbs(tmp_path):
    a, b = tmp_path / "a.db", tmp_path / "b.db"
    _make(b).close()
    return a, b


def test_equal_despite_renumbered_ref_ids_and_timestamp(dbs):
    a, b = dbs
    renumbered = [(10 - r[0],) + r[1:] for r in REFS]
    _make(a, refs=renumbered, config=(("alpha", "0.5"), ("last_computed_at", "999"))).close()
    rep = compare_dbs(a, b)
    assert rep["equal"], rep["differs"]
    # 순서 차이는 정보로만 남는다
    assert rep["info_ref_order"]["same_order"] is False
    assert rep["info_ref_order_in_file"]["same_order"] is False
    assert rep["tables"]["ref"]["rows_a"] == 4


def test_deleted_ref_row_detected(dbs):
    a, b = dbs
    _make(a).execute("DELETE FROM ref WHERE ref_id = 4").connection.commit()
    rep = compare_dbs(a, b)
    assert rep["differs"] == ["ref"]
    r = rep["tables"]["ref"]
    assert (r["only_in_a"], r["only_in_b"]) == (0, 1)
    assert r["examples_b"][0]["name_path"] == "h"
    assert "ref_id" not in r["examples_b"][0]


def test_duplicate_row_counts_as_multiset_difference(dbs):
    a, b = dbs
    # b의 ref 2와 같은 내용(ref_id 빼고)을 하나 더: set으로는 같고 multiset으로는 다르다
    _make(a, refs=REFS + [(5,) + REFS[1][1:]]).close()
    r = compare_dbs(a, b)["tables"]["ref"]
    assert (r["only_in_a"], r["only_in_b"]) == (1, 0)
    assert r["examples_a"][0]["line"] == 4


def test_emits_link_moving_to_other_ref_is_a_ref_difference_not_a_link_difference(dbs):
    a, b = dbs
    moved = [REFS[0][:-1] + (0,), REFS[1][:-1] + (1,)] + REFS[2:]
    _make(a, refs=moved).close()
    rep = compare_dbs(a, b)
    assert rep["differs"] == ["ref"]
    assert rep["tables"]["ref"]["only_in_a"] == 2


def test_null_values_compare_equal(dbs):
    a, b = dbs
    _make(a).close()
    assert compare_dbs(a, b)["tables"]["import_resolution"]["only_in_a"] == 0


def test_link_set_vs_multiset(dbs):
    a, b = dbs
    # 같은 (source, target, kind)의 링크 행이 하나 더: set은 같고 multiset은 다르다
    _make(a, refs=REFS + [(5, "a.py", "a.py::f", "CALLS", "g", 9, "b.py::g", "resolved", 1)]).close()
    rep = compare_dbs(a, b)
    assert rep["tables"]["link_set"]["only_in_a"] == 0
    assert rep["tables"]["link_multiset"]["only_in_a"] == 1
    assert "link_multiset" in rep["differs"] and "link_set" not in rep["differs"]


def test_reading_sequence_one_ulp_detected(dbs):
    a, b = dbs
    rs = [RS[0], RS[1][:5] + (math.nextafter(0.1, 1.0),) + RS[1][6:], RS[2]]
    _make(a, rs=rs).close()
    rep = compare_dbs(a, b)
    assert rep["differs"] == ["reading_sequence"]
    r = rep["tables"]["reading_sequence"]
    assert (r["only_in_a"], r["only_in_b"], r["entities_changed"]) == (1, 1, 1)
    assert r["entities_rank_changed"] == 0
    assert 0 < r["max_abs_diff"]["pagerank_score"] < 1e-16
    assert r["examples_changed"][0]["entity_id"] == "a.py::f"


def test_rank_change_counted(dbs):
    a, b = dbs
    _make(a, rs=[RS[0][:3] + (2,) + RS[0][4:], RS[1], RS[2][:3] + (1,) + RS[2][4:]]).close()
    r = compare_dbs(a, b)["tables"]["reading_sequence"]
    assert r["entities_rank_changed"] == 2


def test_missing_table_and_user_version(dbs):
    a, b = dbs
    conn = _make(a)
    conn.execute("DROP TABLE reading_sequence_config")
    conn.execute("PRAGMA user_version = 7")
    conn.commit()
    conn.close()
    rep = compare_dbs(a, b)
    assert {"user_version", "schema", "reading_sequence_config"} <= set(rep["differs"])
    assert rep["tables"]["reading_sequence_config"] == {"missing_in": "a"}


# ── 편집 op ───────────────────────────────────────────────────────────────────

SRC = '''import os


@deco
def f():
    return 1


class Base:
    x = 1

    def m(self):
        return 2


def g(
    a,
) -> int:
    return a
'''


def test_edits_do_not_touch_hardlinked_original(tmp_path):
    src, tree = tmp_path / "src", tmp_path / "tree"
    src.mkdir()
    (src / "m.py").write_text(SRC)
    link_tree(src, tree)
    assert os.stat(src / "m.py").st_ino == os.stat(tree / "m.py").st_ino
    apply_edit(tree, {"op": "replace", "path": "m.py", "old": "return 1", "new": "return 9"})
    apply_edit(tree, {"op": "append", "path": "m.py", "text": "# end\n"})
    assert (src / "m.py").read_text() == SRC
    assert "return 9" in (tree / "m.py").read_text()


def test_replace_requires_exact_match_count(tmp_path):
    (tmp_path / "m.py").write_text(SRC)
    with pytest.raises(ScenarioError, match="3 matches"):
        apply_edit(tmp_path, {"op": "replace", "path": "m.py", "old": "return", "new": "r"})
    apply_edit(tmp_path, {"op": "replace", "path": "m.py", "old": "return", "new": "yield",
                          "count": 3})


def test_remove_def_with_decorator_and_multiline_signature(tmp_path):
    (tmp_path / "m.py").write_text(SRC)
    apply_edit(tmp_path, {"op": "remove_def", "path": "m.py", "name": "f"})
    apply_edit(tmp_path, {"op": "remove_def", "path": "m.py", "name": "g"})
    apply_edit(tmp_path, {"op": "remove_def", "path": "m.py", "name": "Base.m"})
    text = (tmp_path / "m.py").read_text()
    assert "@deco" not in text and "def f" not in text and "def g" not in text
    assert ") -> int" not in text and "def m" not in text
    assert "class Base:\n    x = 1\n" in text
    with pytest.raises(ScenarioError, match="not found"):
        apply_edit(tmp_path, {"op": "remove_def", "path": "m.py", "name": "nope"})


def test_add_method_indents_into_class_body(tmp_path):
    (tmp_path / "m.py").write_text(SRC)
    apply_edit(tmp_path, {"op": "add_method", "path": "m.py", "class": "Base",
                          "text": "def n(self):\n    return 3\n"})
    text = (tmp_path / "m.py").read_text()
    assert "        return 2\n\n    def n(self):\n        return 3\n" in text
    compile(text, "m.py", "exec")


def test_move_write_remove(tmp_path):
    (tmp_path / "m.py").write_text(SRC)
    apply_edit(tmp_path, {"op": "move", "path": "m.py", "to": "pkg/n.py"})
    apply_edit(tmp_path, {"op": "write", "path": "pkg/new.py", "text": "x = 1\n"})
    apply_edit(tmp_path, {"op": "remove", "path": "pkg/n.py"})
    assert sorted(p.name for p in (tmp_path / "pkg").iterdir()) == ["new.py"]


def test_update_argv(tmp_path):
    db, t = tmp_path / "a.db", tmp_path
    assert update_argv("bin", db, t, {"cmd": "rename", "old": "a.py", "new": "b.py"}) == [
        "bin", "update", "rename", str(db), str(t), "a.py", str(t / "b.py")]
    assert update_argv("bin", db, t, {"cmd": "delete", "file_id": "a.py"}) == [
        "bin", "update", "delete", str(db), "a.py"]


def test_scenario_file_is_well_formed():
    spec = tomllib.loads(DEFAULT_SCENARIOS.read_text(encoding="utf-8"))
    names = [s["name"] for s in spec["scenario"]]
    assert len(names) == len(set(names))
    ops = {"write", "remove", "move", "replace", "append", "remove_def", "add_method"}
    for s in spec["scenario"]:
        assert s["repo"] in spec["repos"], s["name"]
        assert s["edits"] and s["update"], s["name"]
        for e in s.get("setup", []) + s["edits"]:
            assert e["op"] in ops, s["name"]
        for u in s["update"]:
            assert u["cmd"] in {"file", "files", "delete", "rename"}, s["name"]
