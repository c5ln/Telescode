"""`update` 등가성 하네스 — update한 DB가 같은 코드를 새로 스캔한 DB와 같은지 본다.

5단계(증분 재해석) 검증용. 시나리오(`update_scenarios.toml`)마다:

1. 원본 저장소의 .py 파일을 작업 트리로 복사하고 `setup` 편집을 적용한 뒤
   `scan` + `algo` → DB A.
2. `edits`를 트리에 적용하고 `update` 명령을 A에 실행(시간 측정) → `algo` A.
3. 편집된 트리를 새로 `scan` + `algo` → DB B.
4. A와 B를 비교한다. file, symbol, scope, binding, import_resolution, ref(ref_id 제외),
   class/base_class/function/param/field, reading_sequence_config(`last_computed_at`
   제외)는 행 multiset으로, `link` VIEW는 set과 multiset 둘 다로, reading_sequence는
   모든 컬럼을 비트 단위로(REAL은 IEEE 754 비트 패턴; SQLite는 -0.0을 0으로 저장하므로
   ±0은 구분할 수 없다) 비교한다. 표마다 A에만/B에만 있는 행 수와 예시 최대 5개.

편집 전 A와 B의 차이도 `effect`로 남긴다 — 시나리오가 실제로 무언가를 바꾸는지 확인용.

ref 순서도 비교하지만 정보용이다 — 차이로 치지 않는다. 전체 ref_id 순서와 파일 안
ref_id 순서 둘. 함수 그래프는 (kind, file_id, ref_id) 순으로 간선을 넣으므로
(`Graph.cpp build_func_graph`) 파일 안 순서가 다르면 reading_sequence 비트가 바뀔 수 있다.

    bench/.venv/bin/python -m bench.tools.update_equiv \\
        --bin build/TelescodeHeadless --work /tmp/ue --out bench/results/update_stage5/x.json \\
        [--timing] [--only xr_body_edit,syn_*] [--inject ref_row]

`--timing`이면 update를 3번 재서 최솟값을 쓴다(반복마다 A를 다시 만든다: 시작 DB를
복사하거나, `rescan_each_rep` 저장소는 다시 스캔). 비교는 첫 반복에서만 한다.
`--inject`는 하네스 자체 점검용: 비교 직전 A에 일부러 차이를 넣는다.

종료 코드: 모든 시나리오가 같으면 0, 하나라도 다르거나 실행에 실패하면 1.
시간 목표 미달은 표에 표시만 하고 종료 코드에는 반영하지 않는다.

트리 복사는 하드링크를 쓰고, 편집은 항상 새 파일을 쓴 뒤 `os.replace`로 바꾼다
(원본 inode를 건드리지 않는다). 원본 저장소는 먼저 일반 복사로 작업 디렉터리에
떠 두므로 실행 중 원본이 바뀌어도 영향이 없다.
"""

from __future__ import annotations

import argparse
import ast
import fnmatch
import hashlib
import json
import math
import os
import shutil
import sqlite3
import struct
import subprocess
import sys
import time
import tomllib
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SCENARIOS = Path(__file__).with_name("update_scenarios.toml")

# 행 multiset으로 비교하는 표. 열은 PRAGMA table_info에서 읽는다.
ROW_TABLES = ["file", "symbol", "scope", "binding", "import_resolution", "ref",
              "class", "base_class", "function", "param", "field",
              "reading_sequence_config"]
EXCLUDED_COLUMNS = {"ref": {"ref_id"}}
# 비교에서 빼는 행: algo가 실행 시각을 적는다.
ROW_FILTERS = {"reading_sequence_config": "config_key <> 'last_computed_at'"}
# ref_id 순서 비교에 쓰는 열 (정보용)
REF_ORDER_COLUMNS = ["file_id", "source_id", "kind", "name_path", "line"]
# 의존성 표: 자연 키로 바꾼 SELECT ({s} = 스키마)
DEP_SELECTS = {
    "dep_fact": "SELECT key FROM {s}.fact",
    "dep_fact_edge": "SELECT p.key AS parent, c.key AS child FROM {s}.fact_edge e"
                     " JOIN {s}.fact p ON p.fact_id = e.parent_id"
                     " JOIN {s}.fact c ON c.fact_id = e.child_id",
    "dep_ref_dep": "SELECT f.key AS fact, r.source_id, r.kind, r.name_path, r.line"
                   " FROM {s}.ref_dep d JOIN {s}.fact f ON f.fact_id = d.fact_id"
                   " JOIN {s}.ref r ON r.ref_id = d.ref_id",
}
DEP_COLUMNS = {"dep_fact": ["key"], "dep_fact_edge": ["parent", "child"],
               "dep_ref_dep": ["fact", "source_id", "kind", "name_path", "line"]}
DEP_TABLES = {"dep_fact": ["fact"], "dep_fact_edge": ["fact", "fact_edge"],
              "dep_ref_dep": ["fact", "ref_dep", "ref"]}
RS_SCORE_COLUMNS = ["pagerank_score", "bc_score", "combined_score"]
MAX_EXAMPLES = 5


class ScenarioError(Exception):
    pass


# ── 비교 ──────────────────────────────────────────────────────────────────────

def _q(name: str) -> str:
    return '"' + name.replace('"', '""') + '"'


def _columns(conn: sqlite3.Connection, schema: str, table: str) -> list[str]:
    return [r[1] for r in conn.execute(f"PRAGMA {schema}.table_info({_q(table)})")]


def _objects(conn: sqlite3.Connection, schema: str) -> dict[str, tuple[str, str]]:
    return {name: (typ, sql) for typ, name, sql in conn.execute(
        f"SELECT type, name, sql FROM {schema}.sqlite_master WHERE name NOT LIKE 'sqlite_%'")}


def _multiset_diff(conn: sqlite3.Connection, select_a: str, select_b: str,
                   cols: list[str]) -> dict:
    """두 SELECT 결과를 행 multiset으로 비교한다.

    SQL에서 (행, 개수)로 묶은 뒤 EXCEPT로 다른 묶음만 Python으로 가져온다. 어떤 행이
    A쪽 차이 묶음에만 있고 B쪽에 없으면 B 개수는 0이다(B에 다른 개수로 있었다면 그
    묶음도 B쪽 차이에 나온다). GROUP BY와 EXCEPT는 NULL끼리 같다고 본다.
    """
    cl = ", ".join(_q(c) for c in cols)
    ga = f"SELECT {cl}, COUNT(*) FROM ({select_a}) GROUP BY {cl}"
    gb = f"SELECT {cl}, COUNT(*) FROM ({select_b}) GROUP BY {cl}"
    xa = {r[:-1]: r[-1] for r in conn.execute(f"{ga} EXCEPT {gb}")}
    xb = {r[:-1]: r[-1] for r in conn.execute(f"{gb} EXCEPT {ga}")}
    n_a = conn.execute(f"SELECT COUNT(*) FROM ({select_a})").fetchone()[0]
    n_b = conn.execute(f"SELECT COUNT(*) FROM ({select_b})").fetchone()[0]
    return _counter_diff(xa, xb, cols, n_a, n_b)


def _counter_diff(xa: dict, xb: dict, cols: list[str], n_a: int, n_b: int) -> dict:
    only_a = [(k, n - xb.get(k, 0)) for k, n in xa.items() if n - xb.get(k, 0) > 0]
    only_b = [(k, n - xa.get(k, 0)) for k, n in xb.items() if n - xa.get(k, 0) > 0]

    def examples(items):
        items = sorted(items, key=lambda kn: repr(kn[0]))[:MAX_EXAMPLES]
        out = []
        for k, n in items:
            row = dict(zip(cols, k))
            if n > 1:
                row["__count"] = n
            out.append(row)
        return out

    return {
        "rows_a": n_a, "rows_b": n_b,
        "only_in_a": sum(n for _, n in only_a),
        "only_in_b": sum(n for _, n in only_b),
        "examples_a": examples(only_a),
        "examples_b": examples(only_b),
    }


def _float_bits(v):
    if isinstance(v, float):
        return "f" + struct.pack("<d", v).hex()
    return v


def _compare_reading_sequence(conn: sqlite3.Connection, cols: list[str]) -> dict:
    """모든 열을 비트 단위로. REAL은 IEEE 754 비트 패턴으로 비교한다."""
    cl = ", ".join(_q(c) for c in cols)
    rows = {}
    for schema in ("main", "b"):
        rows[schema] = [tuple(_float_bits(v) for v in r)
                        for r in conn.execute(f"SELECT {cl} FROM {schema}.reading_sequence")]
    ca, cb = Counter(rows["main"]), Counter(rows["b"])
    xa = {k: n for k, n in ca.items() if cb.get(k, 0) != n}
    xb = {k: n for k, n in cb.items() if ca.get(k, 0) != n}
    out = _counter_diff(xa, xb, cols, len(rows["main"]), len(rows["b"]))

    # entity_id 기준 요약: 값이 다른 엔터티 수, 점수별 최대 절대 차, 순위가 다른 수
    def by_entity(schema):
        idx = {c: i for i, c in enumerate(cols)}
        return {r[idx["entity_id"]]: r for r in conn.execute(
            f"SELECT {cl} FROM {schema}.reading_sequence")}
    if out["only_in_a"] or out["only_in_b"]:
        ea, eb = by_entity("main"), by_entity("b")
        idx = {c: i for i, c in enumerate(cols)}
        common = ea.keys() & eb.keys()
        changed = [e for e in common
                   if tuple(map(_float_bits, ea[e])) != tuple(map(_float_bits, eb[e]))]
        max_abs = {c: max((abs(ea[e][idx[c]] - eb[e][idx[c]]) for e in changed), default=0.0)
                   for c in RS_SCORE_COLUMNS if c in idx}
        rank_cols = [c for c in ("file_rank", "local_rank") if c in idx]
        out["entities_only_in_a"] = len(ea.keys() - eb.keys())
        out["entities_only_in_b"] = len(eb.keys() - ea.keys())
        out["entities_changed"] = len(changed)
        out["entities_rank_changed"] = sum(
            1 for e in changed if any(ea[e][idx[c]] != eb[e][idx[c]] for c in rank_cols))
        out["max_abs_diff"] = max_abs
        # 예시는 값을 그대로(repr 가능한 float로) 보여 준다
        out["examples_changed"] = [
            {"entity_id": e,
             **{c: [ea[e][idx[c]], eb[e][idx[c]]] for c in cols
                if c != "entity_id" and _float_bits(ea[e][idx[c]]) != _float_bits(eb[e][idx[c]])}}
            for e in sorted(changed)[:MAX_EXAMPLES]]
    return out


def _compare_ref_order(conn: sqlite3.Connection, order_by: str) -> dict:
    """ref를 `order_by` 순으로 읽었을 때 행 순서가 같은지 (정보용)."""
    cols = [c for c in REF_ORDER_COLUMNS if c in _columns(conn, "main", "ref")]
    cl = ", ".join(_q(c) for c in cols)
    ca = conn.execute(f"SELECT {cl} FROM main.ref ORDER BY {order_by}")
    cb = conn.execute(f"SELECT {cl} FROM b.ref ORDER BY {order_by}")
    mismatches, first, i = 0, None, 0
    while True:
        ra, rb = ca.fetchmany(5000), cb.fetchmany(5000)
        if not ra and not rb:
            break
        for j in range(max(len(ra), len(rb))):
            x = ra[j] if j < len(ra) else None
            y = rb[j] if j < len(rb) else None
            if x != y:
                mismatches += 1
                if first is None:
                    first = {"position": i + j, "a": x and dict(zip(cols, x)),
                             "b": y and dict(zip(cols, y))}
        i += max(len(ra), len(rb))
    return {"same_order": mismatches == 0, "positions_differing": mismatches,
            "first_difference": first}


def compare_dbs(path_a: str | Path, path_b: str | Path) -> dict:
    """A와 B를 비교한 결과. `equal`이 False면 어딘가 다르다."""
    conn = sqlite3.connect(f"file:{path_a}?mode=ro", uri=True)
    try:
        conn.execute("PRAGMA temp_store = MEMORY")
        conn.execute("ATTACH DATABASE ? AS b", (f"file:{path_b}?mode=ro",))
        report: dict = {"tables": {}}
        problems: list[str] = []

        va = conn.execute("PRAGMA main.user_version").fetchone()[0]
        vb = conn.execute("PRAGMA b.user_version").fetchone()[0]
        report["user_version"] = [va, vb]
        if va != vb:
            problems.append("user_version")

        oa, ob = _objects(conn, "main"), _objects(conn, "b")
        schema_diff = sorted(n for n in oa.keys() | ob.keys() if oa.get(n) != ob.get(n))
        if schema_diff:
            report["schema_differs"] = schema_diff
            problems.append("schema")

        for t in ROW_TABLES:
            if t not in oa or t not in ob:
                if (t in oa) != (t in ob):
                    report["tables"][t] = {"missing_in": "b" if t in oa else "a"}
                    problems.append(t)
                continue
            cols = [c for c in _columns(conn, "main", t) if c not in EXCLUDED_COLUMNS.get(t, ())]
            where = f" WHERE {ROW_FILTERS[t]}" if t in ROW_FILTERS else ""
            r = _multiset_diff(conn, f"SELECT * FROM main.{_q(t)}{where}",
                               f"SELECT * FROM b.{_q(t)}{where}", cols)
            report["tables"][t] = r
            if r["only_in_a"] or r["only_in_b"]:
                problems.append(t)

        if "link" in oa and "link" in ob:
            cols = ["source_id", "target_id", "link_type"]
            cl = ", ".join(cols)
            ms = _multiset_diff(conn, f"SELECT {cl} FROM main.link",
                                f"SELECT {cl} FROM b.link", cols)
            st = _multiset_diff(conn, f"SELECT DISTINCT {cl} FROM main.link",
                                f"SELECT DISTINCT {cl} FROM b.link", cols)
            report["tables"]["link_multiset"] = ms
            report["tables"]["link_set"] = st
            if ms["only_in_a"] or ms["only_in_b"]:
                problems.append("link_multiset")
            if st["only_in_a"] or st["only_in_b"]:
                problems.append("link_set")

        if "reading_sequence" in oa and "reading_sequence" in ob:
            r = _compare_reading_sequence(conn, _columns(conn, "main", "reading_sequence"))
            report["tables"]["reading_sequence"] = r
            if r["only_in_a"] or r["only_in_b"]:
                problems.append("reading_sequence")

        # 5단계 의존성 기록(fact/fact_edge/ref_dep, 스키마 v7). id는 매번 다시 매기므로
        # 자연 키(fact.key, ref의 UNIQUE 열)로 바꿔 비교한다. 결과 표가 아니라서
        # `differs`가 아니라 `deps_differ`에 적는다: B에만 있는 의존성(update가 잃은 것)은
        # 다음 update가 고쳐야 할 ref를 놓치게 하고, A에만 있는 것은 쓸데없는 재해석만 부른다.
        deps = []
        for t, sel in DEP_SELECTS.items():
            need = DEP_TABLES[t]
            if not all(x in oa and x in ob for x in need):
                continue
            cols = DEP_COLUMNS[t]
            r = _multiset_diff(conn, sel.format(s="main"), sel.format(s="b"), cols)
            report["tables"][t] = r
            if r["only_in_a"] or r["only_in_b"]:
                deps.append(t)
        report["deps_differ"] = deps
        if all(x in oa and x in ob for x in ("fact", "fact_edge", "ref_dep")):
            # 어떤 ref_dep도, 어떤 E fact도 가리키지 않는 fact 수 [A, B]
            report["info_orphan_facts"] = [conn.execute(
                f"SELECT COUNT(*) FROM {sc}.fact WHERE fact_id NOT IN"
                f" (SELECT fact_id FROM {sc}.ref_dep) AND fact_id NOT IN"
                f" (SELECT child_id FROM {sc}.fact_edge)").fetchone()[0] for sc in ("main", "b")]

        if "ref" in oa and "ref" in ob:
            # 전체 ref_id 순서와, Graph.cpp가 함수 그래프 간선을 넣는 순서(파일 안 ref_id)
            report["info_ref_order"] = _compare_ref_order(conn, "ref_id")
            report["info_ref_order_in_file"] = _compare_ref_order(conn, "file_id, ref_id")

        report["differs"] = problems
        report["equal"] = not problems
        return report
    finally:
        conn.close()


def compact(report: dict) -> dict:
    """JSON을 작게: 차이가 없는 표는 행 수만 남긴다."""
    out = dict(report)
    out["tables"] = {}
    for t, r in report.get("tables", {}).items():
        if r.get("only_in_a") or r.get("only_in_b") or "missing_in" in r:
            out["tables"][t] = r
        else:
            out["tables"][t] = {"rows": r["rows_a"]}
    return out


def effect_summary(report: dict) -> dict:
    """편집 전 A와 편집 후 B의 차이: 표마다 [A에만, B에만] 행 수 (차이 있는 표만)."""
    return {t: [r["only_in_a"], r["only_in_b"]] for t, r in report["tables"].items()
            if r.get("only_in_a") or r.get("only_in_b")}


# ── 트리와 편집 ───────────────────────────────────────────────────────────────

def _write(path: Path, text: str) -> None:
    """새 inode에 쓴 뒤 바꾼다 — 하드링크로 공유된 원본을 건드리지 않는다."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".ue_tmp")
    tmp.write_text(text, encoding="utf-8")
    os.replace(tmp, path)


def _find_def(module: ast.Module, dotted: str):
    body, node = module.body, None
    for part in dotted.split("."):
        node = next((n for n in body if isinstance(
            n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)) and n.name == part), None)
        if node is None:
            raise ScenarioError(f"definition not found: {dotted}")
        body = node.body
    return node


def apply_edit(tree: Path, e: dict) -> None:
    op, p = e["op"], tree / e["path"]
    if op == "write":
        _write(p, e["text"])
        return
    if not p.is_file():
        raise ScenarioError(f"{op}: no such file {e['path']}")
    if op == "remove":
        p.unlink()
    elif op == "move":
        dst = tree / e["to"]
        if dst.exists():
            raise ScenarioError(f"move: {e['to']} exists")
        dst.parent.mkdir(parents=True, exist_ok=True)
        os.rename(p, dst)
    elif op == "replace":
        text = p.read_text(encoding="utf-8")
        n, want = text.count(e["old"]), e.get("count", 1)
        if n != want:
            raise ScenarioError(f"replace in {e['path']}: {n} matches, expected {want}")
        _write(p, text.replace(e["old"], e["new"]))
    elif op == "append":
        _write(p, p.read_text(encoding="utf-8") + e["text"])
    elif op in ("remove_def", "add_method"):
        text = p.read_text(encoding="utf-8")
        lines = text.splitlines(keepends=True)
        mod = ast.parse(text)
        if op == "remove_def":
            node = _find_def(mod, e["name"])
            start = min([node.lineno] + [d.lineno for d in node.decorator_list])
            del lines[start - 1:node.end_lineno]
        else:
            node = _find_def(mod, e["class"])
            if not isinstance(node, ast.ClassDef):
                raise ScenarioError(f"add_method: {e['class']} is not a class")
            indent = " " * node.body[0].col_offset
            body = "".join(indent + ln if ln.strip() else ln
                           for ln in e["text"].splitlines(keepends=True))
            lines[node.end_lineno:node.end_lineno] = ["\n", body]
        _write(p, "".join(lines))
    else:
        raise ScenarioError(f"unknown edit op: {op}")


def copy_py_tree(src: Path, dst: Path) -> int:
    """src의 .py 일반 파일만(심볼릭 링크 제외, .git 제외) dst로 복사한다."""
    n = 0
    for dirpath, dirnames, filenames in os.walk(src):
        dirnames[:] = [d for d in dirnames if d != ".git"]
        rel = Path(dirpath).relative_to(src)
        for f in filenames:
            sp = Path(dirpath) / f
            if not f.endswith(".py") or sp.is_symlink() or not sp.is_file():
                continue
            dp = dst / rel / f
            dp.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(sp, dp)
            n += 1
    return n


def link_tree(src: Path, dst: Path) -> None:
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src, dst, copy_function=os.link)


def write_synthetic(files: dict[str, str], dst: Path) -> None:
    for rel, text in files.items():
        _write(dst / rel, text.lstrip("\n"))


# ── 실행 ──────────────────────────────────────────────────────────────────────

def _run(argv: list[str]) -> float:
    t0 = time.perf_counter()
    p = subprocess.run(argv, capture_output=True, text=True)
    dt = time.perf_counter() - t0
    if p.returncode != 0:
        raise ScenarioError(f"{' '.join(argv)} -> exit {p.returncode}: "
                            f"{(p.stderr or p.stdout).strip()[-800:]}")
    return dt


def update_argv(binary: str, db: Path, tree: Path, u: dict) -> list[str]:
    cmd = u["cmd"]
    if cmd == "file":
        return [binary, "update", "file", str(db), str(tree), str(tree / u["path"])]
    if cmd == "files":
        return [binary, "update", "files", str(db), str(tree)] + [str(tree / x) for x in u["paths"]]
    if cmd == "delete":
        return [binary, "update", "delete", str(db), u["file_id"]]
    if cmd == "rename":
        return [binary, "update", "rename", str(db), str(tree), u["old"], str(tree / u["new"])]
    raise ScenarioError(f"unknown update cmd: {cmd}")


def _rm_db(db: Path) -> None:
    for suffix in ("", "-wal", "-shm"):
        Path(str(db) + suffix).unlink(missing_ok=True)


def _copy_db(src: Path, dst: Path) -> None:
    _rm_db(dst)
    for suffix in ("", "-wal"):
        if Path(str(src) + suffix).exists():
            shutil.copyfile(str(src) + suffix, str(dst) + suffix)


def scan(binary: str, tree: Path, db: Path) -> tuple[float, float]:
    _rm_db(db)
    return (_run([binary, "scan", str(tree), str(db)]), _run([binary, "algo", str(db)]))


INJECTIONS = {
    "ref_row": "DELETE FROM ref WHERE ref_id = (SELECT MAX(ref_id) FROM ref)",
    "rs_ulp": None,   # reading_sequence 점수 하나를 1 ulp 바꾼다 (Python에서)
    "ir_status": "UPDATE import_resolution SET status = 'unresolved', resolved_file_id = NULL"
                 " WHERE rowid = (SELECT MIN(rowid) FROM import_resolution WHERE status = 'resolved')",
}


def inject(db: Path, kind: str) -> None:
    conn = sqlite3.connect(db)
    try:
        if kind == "rs_ulp":
            eid, v = conn.execute("SELECT entity_id, pagerank_score FROM reading_sequence"
                                  " ORDER BY entity_id LIMIT 1").fetchone()
            conn.execute("UPDATE reading_sequence SET pagerank_score = ? WHERE entity_id = ?",
                         (math.nextafter(v, math.inf), eid))
        else:
            conn.execute(INJECTIONS[kind])
        conn.commit()
    finally:
        conn.close()


def run_scenario(sc: dict, repo: dict, pristine: Path, binary: str, work: Path,
                 reps: int, base_cache: dict, inject_kind: str | None) -> dict:
    tree = work / "tree"
    db_a, db_b, db_a0 = work / "a.db", work / "b.db", work / f"a0_{sc['name']}.db"
    res: dict = {"name": sc["name"], "repo": sc["repo"], "class": sc.get("class", "other"),
                 "target_ms": sc.get("target_ms")}
    rescan = repo.get("rescan_each_rep", False)
    setup = sc.get("setup", [])

    def fresh_tree():
        link_tree(pristine, tree)
        for e in setup:
            apply_edit(tree, e)

    # 시작 DB(A0): setup이 없으면 저장소마다 한 번만 스캔해 재사용한다.
    if not rescan:
        if setup or sc["repo"] not in base_cache:
            fresh_tree()
            scan(binary, tree, db_a0)
            if not setup:
                base_cache[sc["repo"]] = db_a0
        else:
            db_a0 = base_cache[sc["repo"]]

    times, per_op = [], []
    try:
        for rep in range(reps):
            fresh_tree()
            if rescan:
                scan(binary, tree, db_a)
            else:
                _copy_db(db_a0, db_a)
            for e in sc["edits"]:
                apply_edit(tree, e)
            if rep == 0:
                # B를 먼저 만들고 편집 전 A와 비교해 둔다: 시나리오가 실제로 무언가를
                # 바꾸는지(검사가 헛돌지 않는지) 보는 용도.
                scan_s, _ = scan(binary, tree, db_b)
                res["fresh_scan_ms"] = round(scan_s * 1000, 1)
                res["effect"] = effect_summary(compare_dbs(db_a, db_b))
            ops = [_run(update_argv(binary, db_a, tree, u)) for u in sc["update"]]
            times.append(sum(ops))
            per_op.append(ops)
            if rep == 0:
                res["algo_ms"] = round(_run([binary, "algo", str(db_a)]) * 1000, 1)
                if inject_kind:
                    inject(db_a, inject_kind)
                    res["injected"] = inject_kind
                res["compare"] = compact(compare_dbs(db_a, db_b))
                _rm_db(db_b)
            _rm_db(db_a)
    finally:
        if db_a0 not in base_cache.values():
            _rm_db(db_a0)
    res["update_ms_all"] = [round(t * 1000, 1) for t in times]
    res["update_ms"] = round(min(times) * 1000, 1)
    if len(sc["update"]) > 1:
        res["update_ms_per_op_min"] = [round(min(x) * 1000, 1) for x in zip(*per_op)]
    res["equal"] = res["compare"]["equal"]
    if res["target_ms"] is not None:
        res["meets_target"] = res["update_ms"] <= res["target_ms"]
    return res


def prepare_repo(name: str, repo: dict, work: Path) -> Path:
    pristine = work / "pristine" / name
    if pristine.exists():
        shutil.rmtree(pristine)
    pristine.mkdir(parents=True)
    if "files" in repo:
        write_synthetic(repo["files"], pristine)
    else:
        copy_py_tree((ROOT / repo["path"]).resolve(), pristine)
    return pristine


def _md5(path: str) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def print_table(results: list[dict]) -> None:
    print(f"{'scenario':34} {'class':8} {'update ms':>10} {'target':>7} {'equal':>6}  differs")
    for r in results:
        if "error" in r:
            print(f"{r['name']:34} {r.get('class', ''):8} {'-':>10} {'':>7} {'ERROR':>6}  {r['error'][:90]}")
            continue
        tgt = r["target_ms"]
        mark = "" if tgt is None else ("" if r["meets_target"] else " !")
        diff = ",".join(r["compare"]["differs"])
        if r["compare"].get("deps_differ"):
            diff += (" " if diff else "") + "[deps: " + ",".join(r["compare"]["deps_differ"]) + "]"
        print(f"{r['name']:34} {r['class']:8} {r['update_ms']:>10.1f} "
              f"{'' if tgt is None else tgt:>7}{mark:2} {'yes' if r['equal'] else 'NO':>4}  {diff}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bin", required=True, help="TelescodeHeadless 경로")
    ap.add_argument("--scenarios", default=str(DEFAULT_SCENARIOS))
    ap.add_argument("--work", required=True, help="작업 디렉터리 (트리와 DB를 만든다)")
    ap.add_argument("--out", help="JSON 보고서 경로")
    ap.add_argument("--timing", action="store_true", help="update를 3번 재서 최솟값")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--only", help="쉼표로 구분한 시나리오 이름 (glob 가능)")
    ap.add_argument("--repo", action="append", default=[],
                    help="저장소 경로 덮어쓰기 name=path (예: xarray=/path/to/xarray)")
    ap.add_argument("--inject", choices=sorted(INJECTIONS), help="자체 점검: A에 차이를 넣는다")
    args = ap.parse_args(argv)

    spec = tomllib.loads(Path(args.scenarios).read_text(encoding="utf-8"))
    repos = spec["repos"]
    for kv in args.repo:
        name, _, path = kv.partition("=")
        repos[name] = {**repos.get(name, {}), "path": path}
        repos[name].pop("files", None)
    scenarios = spec["scenario"]
    if args.only:
        pats = args.only.split(",")
        scenarios = [s for s in scenarios if any(fnmatch.fnmatch(s["name"], p) for p in pats)]
    if not scenarios:
        print("no scenarios selected", file=sys.stderr)
        return 1

    binary = str(Path(args.bin).resolve())
    work = Path(args.work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    reps = args.reps if args.timing else 1

    pristine: dict[str, Path] = {}
    base_cache: dict = {}
    results = []
    try:
        for sc in scenarios:
            name = sc["repo"]
            if name not in pristine:
                pristine[name] = prepare_repo(name, repos[name], work)
            try:
                r = run_scenario(sc, repos[name], pristine[name], binary, work, reps,
                                 base_cache, args.inject)
            except ScenarioError as ex:
                r = {"name": sc["name"], "repo": name, "class": sc.get("class", "other"),
                     "error": str(ex), "equal": False}
            results.append(r)
            state = "error" if "error" in r else ("equal" if r["equal"] else "DIFFERS")
            print(f"[{len(results)}/{len(scenarios)}] {sc['name']}: {state}"
                  + (f", update {r['update_ms']} ms" if "update_ms" in r else ""),
                  file=sys.stderr, flush=True)
    finally:
        for db in base_cache.values():
            _rm_db(db)
        shutil.rmtree(work / "tree", ignore_errors=True)
        shutil.rmtree(work / "pristine", ignore_errors=True)

    print_table(results)
    report = {"binary": binary, "binary_md5": _md5(binary), "timing": args.timing,
              "reps": reps, "inject": args.inject,
              "all_equal": all(r["equal"] for r in results), "scenarios": results}
    if args.out:
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(json.dumps(report, indent=1, ensure_ascii=False) + "\n",
                                  encoding="utf-8")
    return 0 if report["all_equal"] else 1


if __name__ == "__main__":
    sys.exit(main())
