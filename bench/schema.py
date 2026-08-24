"""피처 매트릭스 컬럼명의 단일 출처.

문자열 리터럴 대신 이 상수를 import 한다. 오타가 조용한 조인 실패가 아니라
ImportError / AttributeError가 되게 하려는 것이다.

    from bench.schema import C, FEATURE_COLUMNS
    df[C.file_id]        # O
    df["file_id"]        # X

이 파일은 공유 계약이다. 에이전트가 임의로 수정하지 않는다.
컬럼 추가는 가능하되, 기존 컬럼명 변경은 최종 보고서 최상단에 명시할 것.
자세한 내용은 bench/CONTRACT.md 참조.
"""

from pathlib import Path


class C:
    """컬럼명 상수."""

    # ── 키 ────────────────────────────────────────────────────────────────
    instance_id = "instance_id"   # 랭킹 group key이자 train/test 분할 단위
    file_id     = "file_id"       # repo 상대경로. Telescode file.file_id와 동일 정규화

    # ── 라벨 ──────────────────────────────────────────────────────────────
    # 1 = gold patch가 수정한 파일. 0 = **unlabeled** (negative 아님).
    # "수정된 파일"은 "읽어야 할 파일"의 부분집합이므로 0을 negative로 학습하면 안 된다.
    is_positive = "is_positive"

    # ── 그래프 피처 (DB에 이미 적재됨) ────────────────────────────────────
    pagerank  = "pagerank"    # reading_sequence.pagerank_score
    bc        = "bc"          # reading_sequence.bc_score
    combined  = "combined"    # reading_sequence.combined_score
    file_rank = "file_rank"   # reading_sequence.file_rank
    in_deg    = "in_deg"      # link 테이블 집계
    out_deg   = "out_deg"
    ppr       = "ppr"         # Phase 3. Wave 2까지는 비어 있음

    # ── 복잡도 피처 ───────────────────────────────────────────────────────
    complexity  = "complexity"    # file.complexity_score
    max_cc      = "max_cc"
    avg_cc      = "avg_cc"
    max_depth   = "max_depth"
    avg_depth   = "avg_depth"
    logical_loc = "logical_loc"

    # ── 의미 피처 ─────────────────────────────────────────────────────────
    bm25      = "bm25"
    bm25_rank = "bm25_rank"
    embed      = "embed"       # 청크 임베딩 최대 코사인 유사도 (retrieval 산출)
    embed_rank = "embed_rank"  # 인스턴스 내 내림차순 순위. 동점은 비관적(최악)

    # ── 메타 (피처로 쓰지 말 것) ──────────────────────────────────────────
    is_generated = "is_generated"  # 후보 필터링 전용
    commit_skew  = "commit_skew"   # repo당 스냅샷 고정 시 base_commit과의 커밋 거리


KEY_COLUMNS = [C.instance_id, C.file_id]
LABEL_COLUMN = C.is_positive

# ablation은 이 그룹 단위로 제거한다 (bench/CONTRACT.md §2)
GRAPH_FEATURES = [C.pagerank, C.bc, C.combined, C.file_rank,
                  C.in_deg, C.out_deg, C.ppr]
COMPLEXITY_FEATURES = [C.complexity, C.max_cc, C.avg_cc,
                       C.max_depth, C.avg_depth, C.logical_loc]
SEMANTIC_FEATURES = [C.bm25, C.bm25_rank, C.embed, C.embed_rank]

FEATURE_COLUMNS = GRAPH_FEATURES + COMPLEXITY_FEATURES + SEMANTIC_FEATURES

# 학습 입력에서 반드시 제외 — 라벨 유래이거나 피처가 아님
EXCLUDED_FROM_TRAINING = [C.is_generated, C.commit_skew] + KEY_COLUMNS + [LABEL_COLUMN]

ALL_COLUMNS = KEY_COLUMNS + [LABEL_COLUMN] + FEATURE_COLUMNS + [C.is_generated, C.commit_skew]


# ── 경로 ──────────────────────────────────────────────────────────────────
BENCH_ROOT = Path(__file__).resolve().parent
BIN_DIR     = BENCH_ROOT / "bin"      # pin된 바이너리. build/를 직접 부르지 않는다
SCRATCH_DIR = BENCH_ROOT / "scratch"  # 인스턴스별 임시 DB. 추출 직후 삭제
DATA_DIR    = BENCH_ROOT / "data"     # 산출 매트릭스 (CSV)

SCANNER_BIN = BIN_DIR / "TelescodeScanner"
ALGO_BIN    = BIN_DIR / "TelescodeAlgo"


def normalize_file_id(path: str) -> str:
    """repo 상대경로 정규화.

    Telescode의 file_id와 SWE-bench gold 파일 경로를 **같은 규칙으로** 통과시켜야 한다.
    한쪽만 정규화하면 조인이 조용히 0행을 만든다.
    조인 후 반드시 매칭 행 수를 검증할 것 (CONTRACT.md §3).
    """
    p = path.strip().replace("\\", "/")
    while p.startswith("./"):
        p = p[2:]
    return p.lstrip("/")
