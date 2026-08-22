"""인스턴스 DB → 피처 행. CONTRACT.md §1 스키마를 그대로 따른다.

DB는 `base_commit` 시점 트리를 스캔한 결과다 (무결성 규칙 §3).
여기서 새로 계산하는 것은 `in_deg` / `out_deg` 둘뿐이고, 나머지는 전부
`TelescodeAlgo`가 이미 써둔 값을 읽기만 한다.

`in_deg`/`out_deg`는 **`GraphBuilder::build_file_graph`와 동일한 간선 집합**에서
센다 (`src/algo/Graph.cpp`). PageRank가 돈 그래프와 차수가 다르면 두 피처가
서로 모순되는 그래프를 설명하게 된다. 구체적으로:

  - CALLS / INHERITS : `entity_id`에서 `'::'` 앞을 잘라 file_id로 접는다
  - IMPORTS          : target이 모듈명이므로 `resolve_module`로 파일에 해석하고,
                       해석 안 되는 외부 모듈(`os`, `pytest` 등)은 버린다
  - self-loop 제외

차수는 **가중치 없는 distinct 이웃 수**다. 가중 합은 이미 `pagerank`에 반영돼
있으므로, 여기서는 "몇 개의 서로 다른 파일과 붙어 있나"라는 다른 정보를 준다.
"""

import sqlite3
from typing import Iterable

import pandas as pd

from bench.schema import ALL_COLUMNS, C


def _resolve_module(module: str, file_ids: set[str]) -> str | None:
    """`src/algo/Graph.cpp`의 `resolve_module`과 동일한 규칙.

    바뀌면 in_deg/out_deg가 PageRank가 본 그래프와 어긋난다.
    """
    path = module.replace(".", "/")
    for candidate in (path + ".py", path + "/__init__.py"):
        if candidate in file_ids:
            return candidate
    return None


def _file_level_edges(conn: sqlite3.Connection,
                      file_ids: set[str]) -> set[tuple[str, str]]:
    """알고리즘이 쓰는 file-level 간선 집합 (방향 있음, 중복 제거)."""
    edges: set[tuple[str, str]] = set()

    # CALLS / INHERITS: 양쪽 다 file-path 기반 entity_id
    rows = conn.execute(
        "SELECT DISTINCT"
        "   substr(source_id, 1, instr(source_id||'::', '::')-1),"
        "   substr(target_id, 1, instr(target_id||'::', '::')-1)"
        " FROM link WHERE link_type IN ('CALLS', 'INHERITS')"
    )
    for src, tgt in rows:
        if src != tgt and src in file_ids and tgt in file_ids:
            edges.add((src, tgt))

    # IMPORTS: target이 모듈명. 프로젝트 파일로 해석되는 것만 남긴다.
    rows = conn.execute(
        "SELECT DISTINCT"
        "   substr(source_id, 1, instr(source_id||'::', '::')-1), target_id"
        " FROM link WHERE link_type = 'IMPORTS'"
    )
    for src, module in rows:
        if src not in file_ids:
            continue
        tgt = _resolve_module(module, file_ids)
        if tgt and tgt != src:
            edges.add((src, tgt))

    return edges


def extract(db_path, instance_id: str, gold_file_ids: Iterable[str],
            commit_skew: int = 0) -> pd.DataFrame:
    """한 인스턴스의 피처 매트릭스를 만든다.

    Args:
        db_path: 스캔+알고리즘이 끝난 인스턴스 DB.
        instance_id: SWE-bench 인스턴스 ID. group key이자 분할 단위.
        gold_file_ids: 이미 `normalize_file_id`를 통과한 gold 경로.
        commit_skew: 스냅샷과 base_commit의 커밋 거리. 인스턴스별로 직접
            base_commit을 스캔하면 0이다.

    Returns:
        `ALL_COLUMNS` 순서의 DataFrame. 스캔된 모든 파일이 한 행씩 나온다
        (gold만 남기면 랭킹 평가가 불가능하다).
        `ppr` / `bm25` / `bm25_rank`는 Phase 2·3 담당이므로 NaN으로 둔다.
    """
    gold = set(gold_file_ids)
    conn = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    try:
        df = pd.read_sql_query(
            "SELECT"
            "   f.file_id                       AS file_id,"
            "   f.complexity_score              AS complexity,"
            "   f.max_cyclomatic_complexity     AS max_cc,"
            "   f.avg_cyclomatic_complexity     AS avg_cc,"
            "   f.max_block_depth               AS max_depth,"
            "   f.avg_block_depth               AS avg_depth,"
            "   f.logical_loc                   AS logical_loc,"
            "   f.is_generated                  AS is_generated,"
            "   rs.pagerank_score               AS pagerank,"
            "   rs.bc_score                     AS bc,"
            "   rs.combined_score               AS combined,"
            "   rs.file_rank                    AS file_rank"
            " FROM file f"
            " LEFT JOIN reading_sequence rs"
            "   ON rs.entity_id = f.file_id AND rs.entity_type = 'file'",
            conn,
        )
        file_ids = set(df[C.file_id])
        edges = _file_level_edges(conn, file_ids)
    finally:
        conn.close()

    out_deg: dict[str, int] = {}
    in_deg: dict[str, int] = {}
    for src, tgt in edges:
        out_deg[src] = out_deg.get(src, 0) + 1
        in_deg[tgt] = in_deg.get(tgt, 0) + 1

    df[C.instance_id] = instance_id
    df[C.is_positive] = df[C.file_id].isin(gold).astype(int)
    df[C.in_deg] = df[C.file_id].map(in_deg).fillna(0).astype(int)
    df[C.out_deg] = df[C.file_id].map(out_deg).fillna(0).astype(int)
    df[C.ppr] = float("nan")          # Phase 3 (algo-core)
    df[C.bm25] = float("nan")         # Phase 2 (retrieval)
    df[C.bm25_rank] = float("nan")    # Phase 2 (retrieval)
    df[C.commit_skew] = commit_skew

    return df[ALL_COLUMNS]
