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
    assert d["join"] == "class_name"            # target이 이름 → 5e1638d 조인
    assert d["by_join"]["class_id"]["edges"] == 0
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


def _resolved_db():
    """e55c4ea 형태: INHERITS target이 class id, reading_sequence·ref·import_resolution 있음."""
    conn = _db()
    conn.execute("DELETE FROM link WHERE link_type = 'INHERITS'")
    conn.executemany("INSERT INTO link VALUES (?, ?, ?)", [
        ("repos/a/x.py::Child", "repos/a/x.py::Base", "INHERITS"),     # 같은 파일 Base 하나만
        ("repos/b/z.py::Installer", "repos/a/x.py::Base", "INHERITS"),  # 다른 파일·프로젝트
        ("repos/a/x.py::main", "repos/a/x.py::helper", "DECORATES"),
        ("repos/a/y.py", "x", "IMPORTS"),
        ("repos/a/y.py", "os", "IMPORTS"),
    ])
    conn.executescript("""
    CREATE TABLE import_resolution (file_id TEXT, module_spec TEXT,
                                    resolved_file_id TEXT, status TEXT);
    INSERT INTO import_resolution VALUES ('repos/a/y.py', 'x', 'repos/a/x.py', 'resolved'),
                                         ('repos/a/y.py', 'os', NULL, 'external');
    CREATE TABLE reading_sequence (entity_id TEXT, entity_type TEXT);
    INSERT INTO reading_sequence VALUES ('repos/a/x.py', 'file'), ('repos/a/y.py', 'file'),
        ('repos/b/z.py', 'file'), ('repos/a/x.py::Base', 'class'),
        ('repos/a/x.py::main', 'function'), ('repos/a/x.py::helper', 'function');
    CREATE TABLE ref (kind TEXT, status TEXT, confidence TEXT, name_path TEXT);
    INSERT INTO ref VALUES ('CALLS', 'resolved', 'exact', 'helper'),
        ('CALLS', 'resolved', 'heuristic', 'self.open'),
        ('CALLS', 'builtin', 'exact', 'len'),
        ('CALLS', 'unresolved', NULL, 'obj.run'),
        ('CALLS', 'unresolved', NULL, 'obj.stop'),
        ('INHERITS', 'external', 'exact', 'abc.ABC');
    """)
    return conn


def test_class_diagram_id_join():
    d = measure(_resolved_db())["class_diagram_inherits"]
    assert d["join"] == "class_id"
    assert d["inherits_links"] == 2
    assert d["edges"] == 2
    assert d["edges_to_different_file"] == 1
    assert d["edges_to_different_project"] == 1
    assert d["children_with_multiple_parents"] == 0
    assert d["base_names_matching_multiple_classes"] == 0
    assert d["by_join"]["class_name"]["edges"] == 0     # id는 어떤 class_name과도 안 맞는다


def test_file_graph_with_id_inherits_and_imports():
    r = measure(_resolved_db())
    f = r["file_graph"]
    assert f["INHERITS"]["edges"] == 2                  # x→x (self loop), z→x
    assert f["INHERITS"]["self_loops"] == 1
    assert f["IMPORTS"]["distinct_pairs_excl_self"] == 1  # y→x, os는 외부
    # CALLS: x→y, x→z / INHERITS: z→x / IMPORTS: y→x
    assert f["all_types"]["distinct_pairs_excl_self"] == 4
    assert f["all_types"]["cross_project_pairs"] == 2   # x→z, z→x
    assert r["links"]["DECORATES"]["total"] == 1
    assert measure(_db())["file_graph"]["IMPORTS"] is None


def test_reading_sequence_and_refs():
    r = measure(_resolved_db())
    rs = r["reading_sequence"]
    assert rs["file"] == {"in_db": 3, "in_reading_sequence": 3, "coverage": 1.0,
                          "examples_missing": []}
    assert rs["class"]["in_db"] == 4 and rs["class"]["in_reading_sequence"] == 1
    assert rs["function"]["coverage"] == 0.4
    refs = r["refs"]
    assert refs["by_kind"]["CALLS"] == {"builtin": 1, "heuristic": 1,
                                        "resolved": 2, "unresolved": 2}
    assert refs["by_kind"]["INHERITS"] == {"external": 1}
    assert refs["top_unresolved_heads"] == [("obj", 2)]
    assert measure(_db())["refs"] is None


def _view_db():
    """4단계 형태: `link`가 ref + import_resolution 위의 VIEW, IMPORTS target이 file_id."""
    conn = sqlite3.connect(":memory:")
    conn.executescript("""
    CREATE TABLE file (file_id TEXT PRIMARY KEY);
    CREATE TABLE class (class_id TEXT PRIMARY KEY, file_id TEXT, class_name TEXT);
    CREATE TABLE function (function_id TEXT PRIMARY KEY, function_name TEXT, class_id TEXT);
    CREATE TABLE ref (ref_id INTEGER PRIMARY KEY, source_id TEXT, kind TEXT, name_path TEXT,
                      resolved_id TEXT, status TEXT, confidence TEXT, emits_link INTEGER);
    CREATE TABLE import_resolution (file_id TEXT, module_spec TEXT,
                                    resolved_file_id TEXT, status TEXT);
    CREATE VIEW link(source_id, target_id, link_type) AS
      SELECT source_id, resolved_id, kind FROM ref
       WHERE emits_link = 1 AND kind IN ('CALLS', 'INHERITS')
      UNION ALL
      SELECT resolved_id, source_id, kind FROM ref
       WHERE emits_link = 1 AND kind = 'DECORATES'
      UNION ALL
      SELECT DISTINCT file_id, resolved_file_id, 'IMPORTS' FROM import_resolution
       WHERE status = 'resolved';
    INSERT INTO file VALUES ('repos/a/x.py'), ('repos/a/y.py'), ('repos/b/z.py');
    INSERT INTO class VALUES ('repos/a/x.py::Base', 'repos/a/x.py', 'Base'),
                             ('repos/b/z.py::Sub', 'repos/b/z.py', 'Sub');
    INSERT INTO function VALUES ('repos/a/x.py::main', 'main', NULL),
                                ('repos/a/y.py::util', 'util', NULL),
                                ('repos/a/y.py::deco', 'deco', NULL);
    INSERT INTO ref VALUES
        (1, 'repos/a/x.py::main', 'CALLS', 'util', 'repos/a/y.py::util', 'resolved', 'exact', 1),
        (2, 'repos/b/z.py::Sub', 'INHERITS', 'Base', 'repos/a/x.py::Base', 'resolved', 'exact', 1),
        (3, 'repos/a/x.py::main', 'DECORATES', 'deco', 'repos/a/y.py::deco', 'resolved', 'exact', 1),
        (4, 'repos/a/x.py::main', 'CALLS', 'len', NULL, 'builtin', 'exact', 0);
    -- y→x를 두 표기로 import해도 VIEW에는 한 행. z→x, y→y(자기 자신), 외부 os.
    INSERT INTO import_resolution VALUES
        ('repos/a/y.py', '.x', 'repos/a/x.py', 'resolved'),
        ('repos/a/y.py', 'a.x', 'repos/a/x.py', 'resolved'),
        ('repos/a/y.py', '.', 'repos/a/y.py', 'resolved'),
        ('repos/b/z.py', 'a.x', 'repos/a/x.py', 'resolved'),
        ('repos/a/y.py', 'os', NULL, 'external');
    """)
    return conn


def test_file_graph_imports_from_view():
    r = measure(_view_db())
    f = r["file_graph"]
    assert f["IMPORTS"] == {"distinct_pairs_excl_self": 2, "source": "link_view"}  # y→x, z→x
    # CALLS: x→y / INHERITS: z→x / IMPORTS: y→x, z→x (z→x 중복)
    assert f["CALLS+INHERITS"]["distinct_pairs_excl_self"] == 2
    assert f["all_types"]["distinct_pairs_excl_self"] == 3
    assert f["all_types"]["cross_project_pairs"] == 1   # z→x
    assert r["links"]["CALLS"]["total"] == 1
    assert r["links"]["DECORATES"]["total"] == 1
    assert r["class_diagram_inherits"]["join"] == "class_id"
    assert r["class_diagram_inherits"]["edges_to_different_project"] == 1
    # 예전 형식(TABLE)은 계속 import_resolution 조인 경로
    assert measure(_resolved_db())["file_graph"]["IMPORTS"]["source"] == "import_resolution"
