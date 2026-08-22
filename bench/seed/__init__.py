"""BM25 seed retrieval — Phase 2 (`retrieval` 담당).

산출물: `bench.schema.C.bm25`, `C.bm25_rank`.
소비처 두 곳:
  1. LTR의 semantic 피처 (`bench/ltr/`)
  2. Personalized PageRank의 teleport 벡터 (`src/algo/`, `algo-core`)

랭킹 지표는 **여기서 구현하지 않는다.** `bench.metrics.ranking`에서 import 한다
(CONTRACT.md §2). BM25 자체는 지표가 아니라 피처이므로 `bench/seed/bm25.py`에 둔다.

## 사용법

    # 1) seed 산출 -> bench/data/bm25_seed__<condition>.csv
    python -m bench.seed.run_seed --repo pytest-dev/pytest --repo-dir <clone>

    # 하네스 매트릭스가 나오면 후보 집합을 그것으로 고정한다 (조인 손실 0)
    python -m bench.seed.run_seed --repo pytest-dev/pytest --repo-dir <clone> \
        --features-csv bench/data/features.csv

    # 2) 공식 지표
    python -m bench.seed.evaluate

    # 3) 누출·도달성·teleport 진단
    python -m bench.seed.diagnostics

## 모듈

| 모듈 | 역할 |
|---|---|
| `textproc` | 쿼리 전처리. 조건 `full` / `no_trace` / `no_paths` |
| `tokenizer` | 코드 인지 토큰화 (원본 식별자 + subtoken 병기) |
| `bm25` | Okapi BM25 (외부 의존성 없음) |
| `dataset` | SWE-bench 인스턴스 로더 (읽기 전용) |
| `corpus` | `base_commit` 시점 트리에서 문서 구성 |
| `run_seed` | 파이프라인 CLI |
| `evaluate` | `bench.metrics.ranking` 호출부 |
| `teleport` | seed -> PPR teleport `(file_id, weight)` + 계약 검증 |
| `join_check` | 피처 매트릭스 조인 매칭 수 검증 |
| `diagnostics` | 누출률·도달성 원자료 요약 |
| `stub` | 계약 스키마 스텁 (배관 확인 전용, 수치 산출 금지) |
"""
