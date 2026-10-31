"""CALLS / INHERITS 링크 품질 기준선 측정 — 이름 해석 개편(A안) 전후 비교용.

스캔 + algo를 마친 DB 하나를 읽어 JSON 보고서와 짧은 요약(stdout)을 낸다.
**같은 스크립트를 개편 전(Stage 1a)과 후(Stage 6)에 그대로 돌려** 숫자를 비교한다.
DB는 읽기 전용으로 연다.

    build/TelescodeHeadless scan <repo> <db>
    build/TelescodeHeadless algo <db>
    python3 -m bench.tools.measure_links --db <db> --label root \\
        --out bench/results/link_baseline/root.json

표준 라이브러리만 쓰므로 `bench/.venv` 없이 시스템 python3로 돈다.

## 무엇을 재는가

1. 링크 총계 — target이 id(`::` 포함)인지 원시 이름인지.
2. 프로젝트 간 링크 — 프로젝트 = 경로에서 마지막 `repos` 세그먼트와 그 다음
   세그먼트(`repos/rich`, `bench/repos/xarray`), 없으면 첫 세그먼트(`bench`, `src`).
   `bench/repos/<x>`와 `site-packages/<pkg>`도 프로젝트로 치는 것은 요청 규칙의
   확장이다: 레포 루트를 스캔하면 bench용 clone 두 개와 `bench/.venv`의 서드파티
   패키지 수천 파일이 전부 `bench`로 뭉쳐 그 사이 오연결이 안 보인다.
3. **bare 이름 호출이 메서드로 연결된 CALLS (근사)** — DB에는 호출 원문이 없다.
   대신 두 신호를 따로 센다.
   - `builtin_name`: target이 메서드이고 그 이름이 `dir(builtins)`에 있다 (`list`, `open` ...).
   - `other_file`: target이 메서드이고 source와 다른 파일에 있다. 현재 파서에서
     파일을 넘는 CALLS는 `resolveCrossFileCallTargets`(점 없는 이름만 처리)에서만
     나오므로 이 범주는 사실상 "bare 이름 → 다른 파일 메서드"다. 그래도 원문을 본 것이
     아니므로 근사로 표시한다.
   e55c4ea(3단계)부터는 `resolveCrossFileCallTargets`가 없고 다른 파일 메서드
   CALLS는 import/receiver 해석에서 나온다. 그래서 3단계 DB의 `other_file`은
   "bare 이름 → 메서드" 신호가 아니라 그냥 다른 파일 메서드 호출 수다.
4. 클래스 다이어그램 INHERITS — `cd_builder.cpp` Step 6의 조인을 재현.
   5e1638d까지는 `class_name = target_id`, e55c4ea부터는 `class_id = target_id`.
   INHERITS target의 과반이 id면 `class_id` 조인을 쓰고(`join` 키), 두 조인의
   숫자는 `by_join`에 모두 남긴다. 다른 파일 부모, 부모 >1인 자식,
   한 부모 이름이 클래스 >1개로 퍼지는 (자식, 이름) 쌍.
5. 파일 그래프 — `Graph.cpp build_file_graph`의 substr/file_ids 필터를 재현.
   개편 전 INHERITS는 target이 이름이라 전부 탈락한다 (기대값 0).
   IMPORTS 패스는 `link`가 VIEW인 DB(4단계부터)에서는 VIEW의 IMPORTS 행
   (target이 file_id)을 그대로 쓰고, `link`가 TABLE이면 `import_resolution`이
   있는 DB에서만 센다 (근사, 코드 주석 참고).
6. reading_sequence 범위 — entity_type별 행 수와 DB의 파일·클래스·함수 수.
7. ref 상태 분포 — kind별 resolved/builtin/external/unresolved(+heuristic),
   unresolved name_path의 첫 세그먼트 상위 15개. `ref` 테이블이 없으면 None.

`DECORATES`는 1·2번에만 추가로 센다 (기준선 JSON에는 없는 키).

예시는 범주마다 최대 `--examples`개(기본 10), 정렬해서 결정적으로 뽑는다.

## Stage 6 — RESULTS.md 수치 재생성 (여기서는 실행하지 않았다)

`bench/collect/pipeline.py`는 `bench.schema.SCANNER_BIN/ALGO_BIN`
(`bench/bin/TelescodeScanner`, `TelescodeAlgo`, `e2501cb`에서 pin)만 부른다.
**개편된 파서로 재측정하려면 먼저 `bench/bin/`을 다시 pin해야 한다.** 그러지 않으면
옛 알고리즘을 다시 잰다. 현재 CMake는 `TelescodeScanner/Algo` 타깃을 만들지 않고
`TelescodeHeadless scan|algo`만 만든다 — pipeline은 `[SCANNER_BIN, tree, db]`로
부르므로 서브커맨드를 붙이는 래퍼 스크립트를 pin하거나 `bench/schema.py`를 고쳐야
한다. pin 후 `PINNED_FROM.txt`(커밋·md5)를 갱신한다. `TelescodePPR`도 같은 빌드로 다시 pin.

임베딩/BM25/리랭크 seed는 파일 내용만 보고 링크를 보지 않으므로 재사용한다
(API 호출 불필요). 링크가 바뀌면 달라지는 것은 그래프 피처와 PPR뿐이다.

    # 0. 테스트 / clone 확인 (시스템 python3는 PEP 668로 잠김)
    bench/.venv/bin/python -m pytest bench/ -q
    bench/.venv/bin/python bench/fetch_repositories.py --verify-only

    # 1. (선택) 그래프가 바뀌었으니 PPR seed 샤프닝을 train fold에서 다시 고른다
    bench/.venv/bin/python -m bench.features.ppr_sweep \\
        --repo pydata/xarray --repo-dir bench/repos/xarray \\
        --repo pytest-dev/pytest --repo-dir bench/repos/pytest \\
        --seed bench/data/seed_all/embed_seed__no_paths.csv --seed-weight-col embed \\
        --splits bench/data/splits_all.csv --out bench/data/ppr_sweep_embed14.csv
    bench/.venv/bin/python -m bench.features.ppr_select \\
        --sweep bench/data/ppr_sweep_embed14.csv \\
        --splits bench/data/splits_all.csv --manifest bench/data/instances_all.csv

    # 2. 229개 재스캔 + PPR (RESULTS.md의 선택값: topk 1, power 1.0)
    #    SWE-bench parquet은 HF hub에서 받는다 (캐시 없으면 네트워크 필요)
    for spec in pydata/xarray:xarray pytest-dev/pytest:pytest; do
      repo=${spec%%:*}; name=${spec##*:}
      bench/.venv/bin/python -m bench.collect.pipeline \\
        --repo $repo --repo-dir bench/repos/$name --split test \\
        --ppr-bin bench/bin/TelescodePPR \\
        --seed-csv bench/data/seed_all/embed_seed__no_paths.csv \\
        --seed-weight-col embed --seed-topk 1 --seed-power 1.0 \\
        --out bench/data/features_${name}_embedppr.csv \\
        --manifest bench/data/instances_${name}_embedppr.csv
    done
    bench/.venv/bin/python -m bench.features.combine \\
        --features bench/data/features_xarray_embedppr.csv bench/data/features_pytest_embedppr.csv \\
        --manifests bench/data/instances_xarray_embedppr.csv bench/data/instances_pytest_embedppr.csv \\
        --out bench/data/features_all_embedppr.csv \\
        --manifest-out bench/data/instances_all_embedppr.csv \\
        --splits-out bench/data/splits_all.csv
    #    splits_all.csv는 instance_id 해시라 내용이 같아야 한다 — diff로 확인할 것.

    # 3. LTR + ablation (RESULTS.md §1·§3·§6·§8 수치 = ltr_rerank_*)
    bench/.venv/bin/python -m bench.ltr.run_ltr \\
        --features bench/data/features_all_embedppr.csv \\
        --manifest bench/data/instances_all_embedppr.csv \\
        --bm25-seed   bench/data/seed_all/bm25_seed__no_paths.csv \\
        --embed-seed  bench/data/seed_all/embed_seed__no_paths.csv \\
        --rerank-seed bench/data/seed_all/rerank_seed__no_paths.csv \\
        --overlap bench/data/vocab_overlap.csv --overlap-condition no_paths \\
        --splits bench/data/splits_all.csv --out-prefix bench/data/ltr_rerank

`--out-prefix`: RESULTS.md §11은 `ltr_embed`라고 적었지만 §6·§8의 최종 수치는
`ltr_rerank_*`(+`--rerank-seed`)에서 나왔다. 리랭커 이전 단계를 재현하려면
`--rerank-seed` 없이 `--out-prefix bench/data/ltr_embed`.
비교 기준: `bench/data/ltr_rerank.log`, `ltr_rerank_*.csv`의 현재 사본을 덮어쓰기 전에 보관.
"""

from __future__ import annotations

import argparse
import builtins
import json
import sqlite3
import sys
from collections import Counter, defaultdict
from pathlib import Path

APPROX_NOTE = (
    "APPROXIMATION: the DB does not keep the raw call text, so 'bare name call "
    "resolved to a method' is estimated by two separate signals: the target "
    "method's name is a Python builtin (dir(builtins)), or the target method "
    "lives in a different file than the caller."
)


def file_of(entity_id: str) -> str:
    """`pkg/mod.py::Cls::m` → `pkg/mod.py`. Graph.cpp의 substr 규칙과 같다."""
    return entity_id.split("::", 1)[0]


def project_of(path: str) -> str:
    """`repos/rich/x.py` → `repos/rich`, `bench/repos/xarray/a.py` → `bench/repos/xarray`,
    `.../site-packages/pip/x.py` → `.../site-packages/pip`, `src/a.cpp` → `src`.
    세그먼트가 하나뿐이면(루트 파일) 그 자체."""
    parts = path.split("/")
    for i in range(len(parts) - 2, -1, -1):
        if parts[i] in ("repos", "site-packages"):
            return "/".join(parts[:i + 2])
    return parts[0]


def _cap(rows, n):
    return [dict(r) for r in sorted(rows, key=lambda r: tuple(str(v) for v in r.values()))[:n]]


def _ratio(a: int, b: int) -> float | None:
    return round(a / b, 4) if b else None


def _class_diagram(conn, classes, join_col, n_examples):
    """cd_builder Step 6을 `c2.<join_col> = l.target_id` 조인으로 재현한다.
    cd_builder는 두 끝점이 class 테이블에 있는 쌍만 그린다 (classToNodeId)."""
    assert join_col in ("class_name", "class_id")
    cd_rows = conn.execute(
        "SELECT DISTINCT l.source_id, l.target_id, c2.class_id, c2.file_id"
        "  FROM link l"
        f"  JOIN class c2 ON c2.{join_col} = l.target_id"
        " WHERE l.link_type = 'INHERITS'"
        "   AND l.source_id != c2.class_id;").fetchall()
    cd_rows = [r for r in cd_rows if r[0] in classes]
    edges = {(s, c) for s, _, c, _ in cd_rows}
    parents = defaultdict(set)
    per_name = defaultdict(set)
    for s, base, c, _ in cd_rows:
        parents[s].add(c)
        per_name[(s, base)].add(c)
    diff_file = [{"child": s, "base_name": b, "joined_class": c}
                 for s, b, c, f in cd_rows if f != classes[s][1]]
    multi = [{"child": s, "parents": sorted(ps)[:5], "n_parents": len(ps)}
             for s, ps in parents.items() if len(ps) > 1]
    ambiguous = [{"child": s, "base_name": b, "n_classes": len(cs),
                  "joined_classes": sorted(cs)[:5]}
                 for (s, b), cs in per_name.items() if len(cs) > 1]
    matched = {(s, b) for s, b, _, _ in cd_rows}
    return {
        "links_matching_any_class": len(matched),
        "edges": len(edges),
        "edges_to_different_file": len(diff_file),
        "edges_to_different_project": sum(
            1 for d in diff_file
            if project_of(file_of(d["child"])) != project_of(file_of(d["joined_class"]))),
        "children_with_multiple_parents": len(multi),
        "base_names_matching_multiple_classes": len(ambiguous),
        "examples_different_file": _cap(diff_file, n_examples),
        "examples_multiple_parents": sorted(
            multi, key=lambda r: (-r["n_parents"], r["child"]))[:n_examples],
        "examples_ambiguous_base_name": sorted(
            ambiguous, key=lambda r: (-r["n_classes"], r["child"], r["base_name"]))[:n_examples],
    }


def _has_table(conn, name):
    return conn.execute("SELECT 1 FROM sqlite_master WHERE type IN ('table', 'view')"
                        " AND name = ?", (name,)).fetchone() is not None


def _is_view(conn, name):
    """`link`가 VIEW면 4단계 이후 형식 (IMPORTS target이 file_id)."""
    row = conn.execute("SELECT type FROM sqlite_master WHERE name = ?", (name,)).fetchone()
    return row is not None and row[0] == "view"


def measure(conn: sqlite3.Connection, n_examples: int = 10) -> dict:
    builtin_names = set(dir(builtins))

    file_ids = {r[0] for r in conn.execute("SELECT file_id FROM file")}
    classes = {cid: (name, fid) for cid, name, fid in
               conn.execute("SELECT class_id, class_name, file_id FROM class")}
    functions = {fid: (name, cid) for fid, name, cid in
                 conn.execute("SELECT function_id, function_name, class_id FROM function")}

    report: dict = {
        "counts": {
            "files": len(file_ids),
            "classes": len(classes),
            "functions": len(functions),
            "methods": sum(1 for _, c in functions.values() if c is not None),
        },
        "projects": dict(sorted(Counter(project_of(f) for f in file_ids).items())),
    }

    # ── 1. 링크 총계 ────────────────────────────────────────────────────────
    # DECORATES는 기준선 JSON에 없던 키다 (추가만 했다). 파일 그래프에는 안 들어간다.
    links = {"CALLS": [], "INHERITS": [], "DECORATES": []}
    for src, tgt, typ in conn.execute(
            "SELECT source_id, target_id, link_type FROM link"
            " WHERE link_type IN ('CALLS', 'INHERITS', 'DECORATES')"):
        links[typ].append((src, tgt))

    # id = `::` 앞이 스캔된 파일. `::`만 보면 `a[::2]()` 같은 호출 원문도 id로 센다.
    def is_id(t: str) -> bool:
        return "::" in t and file_of(t) in file_ids

    totals = {}
    for typ, rows in links.items():
        ids = [t for _, t in rows if is_id(t)]
        raw = [t for _, t in rows if not is_id(t)]
        totals[typ] = {
            "total": len(rows),
            "target_is_id": len(ids),
            "target_is_raw_name": len(raw),
            "raw_name_dotted": sum(1 for t in raw if "." in t),
            "raw_name_bare": sum(1 for t in raw if "." not in t),
            "raw_name_containing_double_colon": sum(1 for t in raw if "::" in t),
        }
    report["links"] = totals

    # ── 2. 프로젝트 간 링크 (id target만 프로젝트를 안다) ───────────────────
    cross = {}
    for typ, rows in links.items():
        hits = [{"source_id": s, "target_id": t,
                 "source_project": project_of(file_of(s)),
                 "target_project": project_of(file_of(t))}
                for s, t in rows if is_id(t)
                and project_of(file_of(s)) != project_of(file_of(t))]
        pairs = Counter((h["source_project"], h["target_project"]) for h in hits)
        cross[typ] = {
            "count": len(hits),
            "cross_file": sum(1 for s, t in rows if is_id(t) and file_of(s) != file_of(t)),
            "by_project_pair": [{"source": a, "target": b, "count": c}
                                for (a, b), c in pairs.most_common(10)],
            "examples": _cap(hits, n_examples),
        }
    report["cross_project_links"] = cross

    # ── 3. bare 이름 → 메서드 CALLS (근사) ─────────────────────────────────
    builtin_hits, other_file_hits = [], []
    id_not_function = 0
    for s, t in links["CALLS"]:
        if not is_id(t):
            continue
        fn = functions.get(t)
        if fn is None:
            id_not_function += 1
            continue
        name, class_id = fn
        if class_id is None:
            continue
        row = {"source_id": s, "target_id": t, "target_name": name}
        if name in builtin_names:
            builtin_hits.append(row)
        if file_of(s) != file_of(t):
            other_file_hits.append(row)

    def _category(hits):
        return {
            "count": len(hits),
            "cross_project": sum(1 for h in hits if project_of(file_of(h["source_id"]))
                                 != project_of(file_of(h["target_id"]))),
            "top_target_names": Counter(h["target_name"] for h in hits).most_common(10),
            "examples": _cap(hits, n_examples),
        }

    union = {(h["source_id"], h["target_id"]) for h in builtin_hits + other_file_hits}
    report["calls_to_method_from_bare_name"] = {
        "note": APPROX_NOTE,
        "python": sys.version.split()[0],
        "n_builtin_names": len(builtin_names),
        "builtin_name": _category(builtin_hits),
        "other_file": _category(other_file_hits),
        "either": len(union),
        "calls_id_target_not_in_function_table": id_not_function,
    }

    # ── 4. 클래스 다이어그램 INHERITS (cd_builder.cpp Step 6) ───────────────
    # Step 6의 조인은 커밋마다 다르다. 5e1638d까지는 target이 부모 *이름*이라
    # `class_name = target_id`, e55c4ea부터는 해석기가 class id를 쓰므로
    # `class_id = target_id`. DB의 INHERITS target이 id인지 보고 그 DB를 만든
    # 커밋의 조인을 고른다. 두 조인 결과는 `by_join`에 모두 남긴다.
    n_inh = len(links["INHERITS"])
    n_inh_id = totals["INHERITS"]["target_is_id"]
    join = "class_id" if n_inh and n_inh_id * 2 >= n_inh else "class_name"
    by_join = {col: _class_diagram(conn, classes, col, n_examples)
               for col in ("class_name", "class_id")}
    report["class_diagram_inherits"] = {
        "join": join,
        "inherits_links": n_inh,
        **by_join[join],
        "by_join": {col: {k: v for k, v in d.items() if not k.startswith("examples")}
                    for col, d in by_join.items()},
    }

    # ── 5. 파일 그래프 (Graph.cpp build_file_graph, CALLS/INHERITS 패스) ────
    fg = conn.execute(
        "SELECT"
        "    substr(source_id, 1, instr(source_id||'::', '::')-1) AS src,"
        "    substr(target_id, 1, instr(target_id||'::', '::')-1) AS tgt,"
        "    link_type, COUNT(*) AS cnt"
        " FROM link"
        " WHERE link_type IN ('CALLS', 'INHERITS')"
        " GROUP BY src, tgt, link_type;").fetchall()
    kept = [(s, t, typ, n) for s, t, typ, n in fg if s in file_ids and t in file_ids]
    fgraph = {}
    for typ in ("CALLS", "INHERITS"):
        rows = [(s, t, n) for s, t, ty, n in kept if ty == typ]
        fgraph[typ] = {
            "edges": len(rows),
            "links_behind_edges": sum(n for *_, n in rows),
            "self_loops": sum(1 for s, t, _ in rows if s == t),
        }
    pairs = {(s, t) for s, t, _, _ in kept if s != t}
    xproj = sorted((s, t) for s, t in pairs if project_of(s) != project_of(t))
    fgraph["CALLS+INHERITS"] = {
        "distinct_pairs_excl_self": len(pairs),
        "cross_project_pairs": len(xproj),
        "cross_project_ratio": _ratio(len(xproj), len(pairs)),
        "examples_cross_project": [{"src": s, "tgt": t} for s, t in xproj[:n_examples]],
    }
    # IMPORTS 패스는 DB 형식에 따라 다르다.
    # - 4단계부터 `link`는 VIEW이고 IMPORTS 행의 target이 이미 해석된 file_id다
    #   (외부 모듈은 행이 없다). Graph.cpp도 같은 쌍을 import_resolution에서 읽는다.
    # - 그 전(`link`가 TABLE)에는 link.target_id(모듈 이름)를
    #   PythonLanguage::resolveModule로 파일에 붙인다. 파이썬으로 재현하지 않고,
    #   해석기가 남긴 import_resolution(같은 resolveModule 결과)을 쓴다. 그 테이블이
    #   없는 DB(5e1638d)는 None. 근사: Graph.cpp는 link의 module 문자열을 다시
    #   해석하고, import_resolution은 파서가 본 module_spec 기준이다.
    if _is_view(conn, "link"):
        imp = {(s, t) for s, t in conn.execute(
            "SELECT DISTINCT source_id, target_id FROM link WHERE link_type = 'IMPORTS'")
            if s in file_ids and t in file_ids}
        imp_source = "link_view"
    elif _has_table(conn, "import_resolution"):
        imp = {(s, t) for s, t in conn.execute(
            "SELECT DISTINCT ir.file_id, ir.resolved_file_id"
            "  FROM import_resolution ir"
            "  JOIN link l ON l.link_type = 'IMPORTS' AND l.source_id = ir.file_id"
            "             AND l.target_id = ir.module_spec"
            " WHERE ir.resolved_file_id IS NOT NULL")
            if s in file_ids and t in file_ids}
        imp_source = "import_resolution"
    else:
        imp = None
    if imp is not None:
        imp_pairs = {p for p in imp if p[0] != p[1]}
        all_pairs = pairs | imp_pairs
        fgraph["IMPORTS"] = {"distinct_pairs_excl_self": len(imp_pairs),
                             "source": imp_source}
        fgraph["all_types"] = {
            "distinct_pairs_excl_self": len(all_pairs),
            "cross_project_pairs": sum(1 for s, t in all_pairs
                                       if project_of(s) != project_of(t)),
        }
    else:
        fgraph["IMPORTS"] = None
        fgraph["all_types"] = None
    report["file_graph"] = fgraph

    # ── 6. reading_sequence 범위 ────────────────────────────────────────────
    # e55c4ea부터 함수 그래프가 모든 함수·클래스를 노드로 미리 넣는다.
    # 그 전에는 CALLS/INHERITS 링크에 끼지 않은 엔티티가 빠졌다.
    if _has_table(conn, "reading_sequence"):
        rs = dict(conn.execute(
            "SELECT entity_type, COUNT(*) FROM reading_sequence GROUP BY entity_type"))
        rs_ids = {r[0] for r in conn.execute("SELECT entity_id FROM reading_sequence")}
        totals_by_type = {"file": file_ids, "class": set(classes), "function": set(functions)}
        cov = {}
        for typ, ids in totals_by_type.items():
            missing = sorted(ids - rs_ids)
            cov[typ] = {"in_db": len(ids), "in_reading_sequence": rs.get(typ, 0),
                        "coverage": _ratio(len(ids & rs_ids), len(ids)),
                        "examples_missing": missing[:n_examples]}
        report["reading_sequence"] = cov
    else:
        report["reading_sequence"] = None

    # ── 7. ref 상태 분포 (해석기 DB에만 있다) ────────────────────────────────
    if _has_table(conn, "ref"):
        by = defaultdict(Counter)
        for kind, status, conf, n in conn.execute(
                "SELECT kind, status, COALESCE(confidence, '-'), COUNT(*) FROM ref"
                " GROUP BY 1, 2, 3"):
            by[kind][status] += n
            if conf == "heuristic":
                by[kind]["heuristic"] += n
        heads = Counter()
        for (np_,) in conn.execute(
                "SELECT name_path FROM ref WHERE status = 'unresolved'"):
            heads[np_.split(".", 1)[0]] += 1
        report["refs"] = {
            "by_kind": {k: dict(sorted(v.items())) for k, v in sorted(by.items())},
            "note": "'heuristic' is a subset of 'resolved'",
            "top_unresolved_heads": heads.most_common(15),
        }
    else:
        report["refs"] = None
    return report


def summary(report: dict) -> str:
    L, C = report["links"], report["cross_project_links"]
    M, D = report["calls_to_method_from_bare_name"], report["class_diagram_inherits"]
    F = report["file_graph"]
    lines = [
        f"[{report['label']}] files={report['counts']['files']} "
        f"projects={len(report['projects'])}",
        f"  CALLS    total={L['CALLS']['total']} id={L['CALLS']['target_is_id']} "
        f"raw={L['CALLS']['target_is_raw_name']} cross_project={C['CALLS']['count']}",
        f"  INHERITS total={L['INHERITS']['total']} id={L['INHERITS']['target_is_id']} "
        f"raw={L['INHERITS']['target_is_raw_name']} cross_project={C['INHERITS']['count']}",
        f"  CALLS->method (approx): builtin_name={M['builtin_name']['count']} "
        f"other_file={M['other_file']['count']} either={M['either']}",
        f"  class diagram INHERITS (join={D['join']}): edges={D['edges']} diff_file={D['edges_to_different_file']} "
        f"multi_parent_children={D['children_with_multiple_parents']} "
        f"ambiguous_base_names={D['base_names_matching_multiple_classes']}",
        f"  file graph: CALLS edges={F['CALLS']['edges']} INHERITS edges={F['INHERITS']['edges']} "
        f"pairs={F['CALLS+INHERITS']['distinct_pairs_excl_self']} "
        f"cross_project={F['CALLS+INHERITS']['cross_project_pairs']}",
    ]
    R = report.get("reading_sequence")
    if R:
        lines.append("  reading_sequence: " + " ".join(
            f"{t}={v['in_reading_sequence']}/{v['in_db']}" for t, v in R.items()))
    if report.get("refs"):
        for k, v in report["refs"]["by_kind"].items():
            lines.append(f"  refs {k}: " + " ".join(f"{s}={n}" for s, n in v.items()))
    lines.append("  " + APPROX_NOTE)
    return "\n".join(lines)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", required=True, help="scan + algo를 마친 SQLite DB")
    ap.add_argument("--out", required=True, help="JSON 보고서 경로")
    ap.add_argument("--label", default=None, help="보고서 이름 (기본: DB 파일명)")
    ap.add_argument("--scanned-path", default=None, help="스캔한 repo 경로 (기록용)")
    ap.add_argument("--binary", default=None, help="스캔에 쓴 바이너리 경로 (md5 기록용)")
    ap.add_argument("--examples", type=int, default=10)
    args = ap.parse_args(argv)

    db = Path(args.db)
    if not db.exists():
        print(f"DB 없음: {db}", file=sys.stderr)
        return 2
    conn = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    try:
        body = measure(conn, args.examples)
    finally:
        conn.close()

    meta = {"label": args.label or db.stem, "db": db.name,
            "scanned_path": args.scanned_path}
    if args.scanned_path:
        # Stage 6은 같은 커밋을 다시 스캔해야 비교가 성립한다.
        import subprocess
        git = subprocess.run(["git", "-C", args.scanned_path, "rev-parse", "HEAD"],
                             capture_output=True, text=True)
        meta["scanned_commit"] = git.stdout.strip() if git.returncode == 0 else None
    if args.binary:
        import hashlib
        meta["binary"] = {"path": args.binary,
                          "md5": hashlib.md5(Path(args.binary).read_bytes()).hexdigest()}
    report = {**meta, **body}

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    print(summary(report))
    print(f"  → {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
