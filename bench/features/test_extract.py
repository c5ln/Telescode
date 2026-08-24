"""`extract.py`의 모듈 해석이 `src/algo/Graph.cpp`와 갈라지지 않는지 지킨다.

## 왜 이 파일이 필요한가

`in_deg`/`out_deg`는 Python이 다시 센다. `resolveModule`/`inferPackageRoots`가
C++과 어긋나면 **차수가 PageRank가 본 그래프와 조용히 불일치**한다.
크래시도 예외도 없고, 두 피처가 서로 다른 그래프를 설명하는 매트릭스가 나온다.
LTR이 그걸 학습하면 원인 추적이 사실상 불가능하다.

## 어떻게 지키는가

아래 `CPP_EXPECTATIONS`는 `tests/algo/test_module_resolution.cpp`의 기대값을
**그대로 옮긴 것**이다. 같은 35건이 양쪽에서 각각 고정되므로,
어느 한쪽 구현이 움직이면 둘 중 한 스위트가 깨진다. 그게 이 파일의 핵심이다.

  C++ 쪽이 바뀌면  → tests/algo/test_module_resolution.cpp 가 깨진다
  Python 쪽이 바뀌면 → 이 파일이 깨진다

기대값을 고쳐서 통과시키지 마라. 양쪽을 함께 고치거나, 왜 갈라져도 되는지를
근거와 함께 남겨라.

    bench/.venv/bin/python -m pytest bench/features/test_extract.py -q
"""

import sqlite3

import pandas as pd
import pytest

from bench.features.extract import (
    _file_level_edges,
    _infer_package_roots,
    _resolve_module,
    extract,
)
from bench.schema import ALL_COLUMNS, C

# ══════════════════════════════════════════════════════════════════════════
# tests/algo/test_module_resolution.cpp 의 픽스처
# ══════════════════════════════════════════════════════════════════════════

PKG = {
    "pkg/__init__.py",
    "pkg/utils.py",
    "pkg/core/__init__.py",
    "pkg/core/dataset.py",
    "pkg/core/variable.py",
    "absmod.py",
}

SRC_LAYOUT = {
    "src/pytest.py",
    "src/_pytest/__init__.py",
    "src/_pytest/logging.py",
    "src/_pytest/config/__init__.py",
    "src/_pytest/mark/__init__.py",
    "src/_pytest/mark/structures.py",
    "testing/test_logging.py",
    "setup.py",
}

# (file_ids, module, source_file, 기대 결과). None = 해석 실패(C++의 "").
# 순서와 주석은 C++ 테스트의 TEST() 블록을 따른다.
CPP_EXPECTATIONS = [
    # ── 상대 임포트 ────────────────────────────────────────────────────
    (PKG, ".variable", "pkg/core/dataset.py", "pkg/core/variable.py"),
    (PKG, ".core", "pkg/__init__.py", "pkg/core/__init__.py"),
    (PKG, ".utils", "pkg/__init__.py", "pkg/utils.py"),
    (PKG, "..utils", "pkg/core/dataset.py", "pkg/utils.py"),
    # `from . import x` — 파서가 점 하나로 기록한다
    (PKG, ".", "pkg/core/dataset.py", "pkg/core/__init__.py"),
    (PKG, ".", "pkg/utils.py", "pkg/__init__.py"),
    # 패키지 자신의 __init__.py에서 "."은 자기 자신 → self-edge, add_edge가 버린다
    (PKG, ".", "pkg/__init__.py", "pkg/__init__.py"),
    (PKG, "..", "pkg/core/dataset.py", "pkg/__init__.py"),
    (PKG, ".core.dataset", "pkg/__init__.py", "pkg/core/dataset.py"),
    (PKG, "..core.variable", "pkg/core/dataset.py", "pkg/core/variable.py"),
    # ── repo 루트 위로 올라가기 → 해석 실패(오류 아님) ──────────────────
    (PKG, "..utils", "absmod.py", None),
    (PKG, "...utils", "pkg/core/dataset.py", None),
    (PKG, "..", "absmod.py", None),
    (PKG, ".", "absmod.py", None),
    (PKG, ".nosuch", "pkg/core/dataset.py", None),
    # ── 절대 임포트 ────────────────────────────────────────────────────
    (PKG, "pkg.core.dataset", "absmod.py", "pkg/core/dataset.py"),
    (PKG, "pkg.utils", "absmod.py", "pkg/utils.py"),
    (PKG, "pkg", "absmod.py", "pkg/__init__.py"),
    (PKG, "pkg.core", "absmod.py", "pkg/core/__init__.py"),
    # ── 외부 모듈은 계속 미해석 ────────────────────────────────────────
    (PKG, "os", "pkg/core/dataset.py", None),
    (PKG, "sys", "absmod.py", None),
    (PKG, "numpy", "pkg/__init__.py", None),
    (PKG, "os.path", "absmod.py", None),
    (PKG, "", "pkg/core/dataset.py", None),
    # ── src-layout 절대 임포트 ─────────────────────────────────────────
    (SRC_LAYOUT, "_pytest.logging", "testing/test_logging.py", "src/_pytest/logging.py"),
    (SRC_LAYOUT, "_pytest.config", "src/pytest.py", "src/_pytest/config/__init__.py"),
    (SRC_LAYOUT, "pytest", "testing/test_logging.py", "src/pytest.py"),
    (SRC_LAYOUT, "_pytest.mark.structures", "src/pytest.py",
     "src/_pytest/mark/structures.py"),
    # 중첩 패키지는 루트가 아니다 — 아니면 "mark" 단독이 해석돼버린다
    (SRC_LAYOUT, "mark", "src/pytest.py", None),
    # ── src-layout 상대 임포트 (src/가 개입하지 않는다) ────────────────
    (SRC_LAYOUT, ".structures", "src/_pytest/mark/__init__.py",
     "src/_pytest/mark/structures.py"),
    (SRC_LAYOUT, "..logging", "src/_pytest/mark/structures.py", "src/_pytest/logging.py"),
    (SRC_LAYOUT, ".", "src/_pytest/logging.py", "src/_pytest/__init__.py"),
    # ── src-layout에서도 외부 모듈은 거부 ──────────────────────────────
    (SRC_LAYOUT, "os", "src/pytest.py", None),
    (SRC_LAYOUT, "attr", "src/pytest.py", None),
    (SRC_LAYOUT, "numpy", "src/pytest.py", None),
]


def _resolve(file_ids, module, source):
    return _resolve_module(module, source, file_ids, _infer_package_roots(file_ids))


@pytest.mark.parametrize("file_ids,module,source,expected", CPP_EXPECTATIONS)
def test_matches_cpp_expectations(file_ids, module, source, expected):
    """C++ 기대값 35건과 1:1로 일치해야 한다."""
    assert _resolve(file_ids, module, source) == expected


def test_cpp_expectation_count_is_pinned():
    """건수를 박아둔다. 케이스가 빠져도 '전부 통과'로 보이는 걸 막는다."""
    assert len(CPP_EXPECTATIONS) == 35


# ══════════════════════════════════════════════════════════════════════════
# inferPackageRoots — C++ TEST(InferPackageRoots, ...) 와 동일
# ══════════════════════════════════════════════════════════════════════════

def test_flat_repo_has_only_the_repo_root():
    assert _infer_package_roots(PKG) == [""]


def test_src_layout_is_discovered():
    assert _infer_package_roots(SRC_LAYOUT) == ["", "src/"]


def test_repo_root_always_comes_first():
    # ""가 먼저여야 root-layout repo가 이 기능 없던 때와 똑같이 해석된다.
    assert _infer_package_roots(SRC_LAYOUT)[0] == ""


def test_nested_packages_are_not_roots():
    assert "src/_pytest/" not in _infer_package_roots(SRC_LAYOUT)


def test_depth_cap_keeps_fixture_trees_out():
    ids = {"src/app/__init__.py",
           "testing/fixtures/deep/nested/proj/__init__.py"}
    # "testing/fixtures/deep/nested/"는 4단계라 루트가 되면 안 된다.
    assert _infer_package_roots(ids, max_depth=1) == ["", "src/"]


# ══════════════════════════════════════════════════════════════════════════
# 퍼징 — 감사가 40,000 케이스로 불일치 0을 확인했다.
# 여기서는 그 성질들을 불변식으로 고정한다. C++을 호출할 수 없으므로
# 이건 **Python 쪽 회귀**를 잡는 것이고, C++ 쪽은 위 35건이 잡는다.
# ══════════════════════════════════════════════════════════════════════════

def _fuzz_corpus(n: int, seed: int = 20260824):
    import random

    rng = random.Random(seed)
    dirs = ["", "src/", "lib/", "pkg/", "src/app/", "pkg/core/", "a/b/c/"]
    names = ["mod", "utils", "core", "dataset", "logging", "__init__", "x"]
    for _ in range(n):
        ids = set()
        for _ in range(rng.randint(1, 8)):
            d = rng.choice(dirs)
            ids.add(f"{d}{rng.choice(names)}.py")
            if rng.random() < 0.5 and d:
                ids.add(f"{d}__init__.py")
        dots = "." * rng.randint(0, 3)
        tail = ".".join(rng.choice(names) for _ in range(rng.randint(0, 2)))
        module = dots + tail
        source = rng.choice(sorted(ids))
        yield ids, module, source


def test_fuzz_invariants_hold():
    """40,000 케이스. 해석 결과는 항상 실재하는 파일이거나 None이어야 한다."""
    n_resolved = 0
    for ids, module, source in _fuzz_corpus(40_000):
        roots = _infer_package_roots(ids)
        got = _resolve_module(module, source, ids, roots)
        if got is None:
            continue
        n_resolved += 1
        # 1. 존재하지 않는 경로를 지어내지 않는다
        assert got in ids, f"{module!r} from {source!r} → {got!r} (없는 파일)"
        # 2. 루트는 항상 ""를 포함하고 짧은 것부터다
        assert roots[0] == ""
        assert roots == sorted(set(roots), key=lambda r: (len(r), r))
    # 전부 None이면 불변식이 공허하게 통과한다
    assert n_resolved > 1000, f"해석된 케이스가 너무 적다: {n_resolved}"


def test_fuzz_relative_never_escapes_repo_root():
    """점이 디렉토리 깊이보다 많으면 반드시 해석 실패다."""
    for ids, module, source in _fuzz_corpus(5_000):
        if not module.startswith("."):
            continue
        dots = len(module) - len(module.lstrip("."))
        depth = source.count("/")
        if dots - 1 > depth:
            got = _resolve_module(module, source, ids, _infer_package_roots(ids))
            assert got is None, f"{module!r} from {source!r} → {got!r} (루트 위로 탈출)"


# ══════════════════════════════════════════════════════════════════════════
# 간선 집합 — GraphBuilder::build_file_graph 와 같은 규칙인가
# ══════════════════════════════════════════════════════════════════════════

def _db_with(files, links):
    conn = sqlite3.connect(":memory:")
    conn.execute("CREATE TABLE file (file_id TEXT PRIMARY KEY)")
    conn.execute("CREATE TABLE link (source_id TEXT, target_id TEXT, link_type TEXT)")
    conn.executemany("INSERT INTO file VALUES (?)", [(f,) for f in files])
    conn.executemany("INSERT INTO link VALUES (?,?,?)", links)
    return conn


def test_edges_drop_self_loops_and_externals():
    files = ["pkg/__init__.py", "pkg/a.py", "pkg/b.py"]
    conn = _db_with(files, [
        ("pkg/a.py::f", "pkg/b.py::g", "CALLS"),       # 유지
        ("pkg/a.py::f", "pkg/a.py::h", "CALLS"),       # self-loop → 제외
        ("pkg/a.py", "os", "IMPORTS"),                 # 외부 → 제외
        ("pkg/a.py", ".b", "IMPORTS"),                 # 상대 → pkg/b.py
        ("pkg/a.py", "nosuch.mod", "IMPORTS"),         # 미해석 → 제외
    ])
    edges = _file_level_edges(conn, set(files))
    assert edges == {("pkg/a.py", "pkg/b.py")}


def test_edges_collapse_entity_ids_to_files():
    # 서로 다른 함수 쌍이 여러 개여도 파일 간선은 하나로 접힌다.
    files = ["pkg/a.py", "pkg/b.py"]
    conn = _db_with(files, [
        ("pkg/a.py::f", "pkg/b.py::g", "CALLS"),
        ("pkg/a.py::h", "pkg/b.py::i", "CALLS"),
        ("pkg/a.py::C", "pkg/b.py::D", "INHERITS"),
    ])
    assert _file_level_edges(conn, set(files)) == {("pkg/a.py", "pkg/b.py")}


def test_edges_are_directed():
    files = ["pkg/a.py", "pkg/b.py"]
    conn = _db_with(files, [("pkg/a.py::f", "pkg/b.py::g", "CALLS")])
    edges = _file_level_edges(conn, set(files))
    assert ("pkg/a.py", "pkg/b.py") in edges
    assert ("pkg/b.py", "pkg/a.py") not in edges


# ══════════════════════════════════════════════════════════════════════════
# extract() 자체
# ══════════════════════════════════════════════════════════════════════════

@pytest.fixture
def tiny_db(tmp_path):
    """스캐너+algo가 끝난 DB의 최소 재현."""
    path = tmp_path / "t.db"
    conn = sqlite3.connect(path)
    conn.executescript("""
        CREATE TABLE file (
            file_id TEXT PRIMARY KEY, file_name TEXT, language TEXT,
            raw_loc INT, logical_loc INT, is_generated INT,
            max_cyclomatic_complexity INT, avg_cyclomatic_complexity REAL,
            max_block_depth INT, avg_block_depth REAL,
            max_function_loc INT, avg_function_loc REAL, complexity_score REAL);
        CREATE TABLE link (source_id TEXT, target_id TEXT, link_type TEXT);
        CREATE TABLE reading_sequence (
            entity_id TEXT PRIMARY KEY, entity_type TEXT, file_id TEXT,
            file_rank INT, local_rank INT,
            pagerank_score REAL, bc_score REAL, combined_score REAL);
    """)
    for i, f in enumerate(["pkg/a.py", "pkg/b.py"], start=1):
        conn.execute("INSERT INTO file VALUES (?,?,'python',10,8,0,3,1.5,2,1.0,5,4.0,?)",
                     (f, f, 0.5 * i))
        conn.execute("INSERT INTO reading_sequence VALUES (?,'file',?,?,NULL,?,?,?)",
                     (f, f, i, 0.1 * i, 2.0 * i, 0.3 * i))
    conn.execute("INSERT INTO link VALUES ('pkg/a.py::f','pkg/b.py::g','CALLS')")
    conn.commit()
    conn.close()
    return path


def test_extract_schema_and_labels(tiny_db):
    df = extract(tiny_db, "inst-1", {"pkg/b.py"})
    # 계약 §1: 컬럼 순서까지 ALL_COLUMNS와 같아야 한다
    assert list(df.columns) == ALL_COLUMNS
    assert len(df) == 2
    assert set(df[df[C.is_positive] == 1][C.file_id]) == {"pkg/b.py"}
    # a → b 간선 하나
    a = df.set_index(C.file_id).loc["pkg/a.py"]
    b = df.set_index(C.file_id).loc["pkg/b.py"]
    assert (a[C.out_deg], a[C.in_deg]) == (1, 0)
    assert (b[C.out_deg], b[C.in_deg]) == (0, 1)


def test_extract_gold_outside_scan_is_simply_absent(tiny_db):
    # 스캔에 없는 gold는 행이 생기지 않는다. Recall 분모는 manifest가 책임진다.
    df = extract(tiny_db, "inst-1", {"pkg/b.py", "brand/new.py"})
    assert len(df) == 2
    assert int(df[C.is_positive].sum()) == 1


def test_extract_ppr_nan_means_not_run(tiny_db):
    """미실행 NaN과 미도달 0.0은 다른 사실이다."""
    assert extract(tiny_db, "i", set())[C.ppr].isna().all()

    got = extract(tiny_db, "i", set(), ppr_values={"pkg/a.py": 0.7})
    s = got.set_index(C.file_id)[C.ppr]
    assert s["pkg/a.py"] == pytest.approx(0.7)
    assert s["pkg/b.py"] == 0.0          # 도달 못 함 = 측정된 0
    assert not s.isna().any()


def test_extract_leaves_phase2_columns_empty(tiny_db):
    df = extract(tiny_db, "i", set())
    assert df[C.bm25].isna().all()
    assert df[C.bm25_rank].isna().all()
