"""measure_links — 손으로 센 소형 DB로 각 범주 숫자를 고정한다."""

import sqlite3

from bench.tools.measure_links import measure, project_of

SCHEMA = """
CREATE TABLE file (file_id TEXT PRIMARY KEY);
CREATE TABLE class (class_id TEXT PRIMARY KEY, file_id TEXT, class_name TEXT);
CREATE TABLE function (function_id TEXT PRIMARY KEY, function_name TEXT, class_id TEXT);
CREATE TABLE link (source_id TEXT, target_id TEXT, link_type TEXT,
                   PRIMARY KEY (source_id, target_id, link_type));
"""


def _db():
    conn = sqlite3.connect(":memory:")
    conn.executescript(SCHEMA)
    conn.executemany("INSERT INTO file VALUES (?)", [
        ("repos/a/x.py",), ("repos/a/y.py",), ("repos/b/z.py",)])
    conn.executemany("INSERT INTO class VALUES (?, ?, ?)", [
        ("repos/a/x.py::Base", "repos/a/x.py", "Base"),
        ("repos/a/x.py::Child", "repos/a/x.py", "Child"),
        ("repos/b/z.py::Base", "repos/b/z.py", "Base"),
        ("repos/b/z.py::Installer", "repos/b/z.py", "Installer"),
    ])
    conn.executemany("INSERT INTO function VALUES (?, ?, ?)", [
        ("repos/a/x.py::main", "main", None),
        ("repos/a/x.py::helper", "helper", None),
        ("repos/a/y.py::util", "util", None),
        ("repos/b/z.py::Installer::list", "list", "repos/b/z.py::Installer"),
        ("repos/a/x.py::Child::open", "open", "repos/a/x.py::Child"),
    ])
    conn.executemany("INSERT INTO link VALUES (?, ?, ?)", [
        ("repos/a/x.py::main", "repos/a/x.py::helper", "CALLS"),           # 같은 파일 함수
        ("repos/a/x.py::main", "repos/a/y.py::util", "CALLS"),             # 다른 파일, 같은 프로젝트
        ("repos/a/x.py::main", "repos/b/z.py::Installer::list", "CALLS"),  # 내장 이름 + 다른 파일 메서드
        ("repos/a/x.py::main", "repos/a/x.py::Child::open", "CALLS"),      # 내장 이름, 같은 파일
        ("repos/a/x.py::main", "os.path.join", "CALLS"),
        ("repos/a/x.py::main", "a[::2]", "CALLS"),                         # `::` 있는 원문
        ("repos/a/x.py::Child", "Base", "INHERITS"),                       # 두 클래스로 퍼진다
        ("repos/a/x.py::Child", "object", "INHERITS"),                     # 매칭 없음
    ])
    return conn


def test_project_of():
    assert project_of("repos/rich/rich/x.py") == "repos/rich"
    assert project_of("bench/repos/xarray/xarray/a.py") == "bench/repos/xarray"
    assert project_of("bench/.venv/lib/python3.11/site-packages/pip/x.py") == \
        "bench/.venv/lib/python3.11/site-packages/pip"
    assert project_of("src/a.cpp") == "src"
    assert project_of("conftest.py") == "conftest.py"


def test_link_totals():
    r = measure(_db())
    calls = r["links"]["CALLS"]
    assert calls == {"total": 6, "target_is_id": 4, "target_is_raw_name": 2,
                     "raw_name_dotted": 1, "raw_name_bare": 1,
                     "raw_name_containing_double_colon": 1}
    assert r["links"]["INHERITS"]["target_is_id"] == 0


def test_cross_project_and_methods():
    r = measure(_db())
    assert r["cross_project_links"]["CALLS"]["count"] == 1
    assert r["cross_project_links"]["CALLS"]["cross_file"] == 2
    m = r["calls_to_method_from_bare_name"]
    assert m["builtin_name"]["count"] == 2      # list, open
    assert m["other_file"]["count"] == 1        # Installer::list
    assert m["other_file"]["cross_project"] == 1
    assert m["either"] == 2


def test_class_diagram_join():
    d = measure(_db())["class_diagram_inherits"]
    assert d["inherits_links"] == 2
    assert d["links_matching_any_class"] == 1
    assert d["edges"] == 2
    assert d["edges_to_different_file"] == 1
    assert d["edges_to_different_project"] == 1
    assert d["children_with_multiple_parents"] == 1
    assert d["base_names_matching_multiple_classes"] == 1


def test_file_graph_drops_name_targets():
    f = measure(_db())["file_graph"]
    assert f["INHERITS"]["edges"] == 0
    assert f["CALLS"]["edges"] == 3             # x→x (helper·open 합침), x→y, x→z
    assert f["CALLS"]["self_loops"] == 1
    assert f["CALLS+INHERITS"]["distinct_pairs_excl_self"] == 2
    assert f["CALLS+INHERITS"]["cross_project_pairs"] == 1
