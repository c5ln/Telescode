# bench/ 계약 — 에이전트 간 인터페이스

Issue #37 추천 알고리즘 고도화 실험의 **공유 계약**이다.
여러 에이전트가 병렬로 일하므로, 여기 적힌 것은 임의로 바꾸지 않는다.
바꿔야 하면 **최종 보고서 최상단에 명시**한다.

---

## 디렉토리 소유권

| 경로 | 소유자 | 다른 에이전트 |
|---|---|---|
| `bench/schema.py` | **공유 — 수정 금지** | 읽기·import만 |
| `bench/metrics/` | `bench-harness`가 구현 | 나머지는 **import해서 사용**. 자체 구현 금지 |
| `bench/collect/` | `bench-harness` | 읽기만 |
| `bench/features/` | `bench-harness` | 읽기만 |
| `bench/seed/` | `retrieval` | 읽기만 |
| `bench/ltr/` | `ltr-eval` | 읽기만 |
| `bench/bin/` | pin된 바이너리. Wave 1 종료 후 고정 | 실행만 |
| `bench/scratch/` | 인스턴스별 임시 DB. **추출 직후 삭제** | — |
| `bench/data/` | 산출 매트릭스 (CSV) | 읽기 |
| `src/` | `algo-core` | 읽기만 |

---

## 1. 피처 매트릭스 스키마

컬럼명의 단일 출처는 **`bench/schema.py`**다. 문자열 리터럴을 직접 쓰지 말고 상수를 import 한다.
오타가 조용한 조인 실패가 아니라 `ImportError`가 되게 하려는 것이다.

```python
from bench.schema import C, FEATURE_COLUMNS, GRAPH_FEATURES
df[C.file_id]        # O
df["file_id"]        # X
```

저장 형식: **CSV**, `bench/data/`. 100 issue × ~700 file ≈ 70k 행이라 성능 문제 없고, 사람이 직접 열어 검증할 수 있다.

### 키

| 컬럼 | 의미 |
|---|---|
| `instance_id` | SWE-bench 인스턴스 ID. **랭킹 group key이자 train/test 분할 단위** |
| `file_id` | repo 상대경로. Telescode `file.file_id`와 동일 정규화 |

### 라벨

| 컬럼 | 의미 |
|---|---|
| `is_positive` | gold patch가 수정한 파일이면 1, 아니면 0 |

> ⚠ **`is_positive == 0`은 negative가 아니라 `unlabeled`다.**
> "수정된 파일"은 "읽어야 할 파일"의 부분집합이다(1개 고치려고 10개 읽는다).
> 0을 negative로 학습에 쓰면 모델이 *"구조적 맥락은 필요 없다"*를 학습해
> 이 프로젝트의 주장과 정반대 결론이 나온다. positive-unlabeled로 다룬다.

### 그래프 피처 (전부 DB에 이미 적재됨 — 추가 계산 없음)

| 컬럼 | 출처 |
|---|---|
| `pagerank` | `reading_sequence.pagerank_score` |
| `bc` | `reading_sequence.bc_score` |
| `combined` | `reading_sequence.combined_score` |
| `file_rank` | `reading_sequence.file_rank` |
| `in_deg` / `out_deg` | `link` 테이블 집계 |
| `ppr` | Phase 3 Personalized PageRank. Wave 2까지는 비어 있음 |

### 복잡도 피처

| 컬럼 | 출처 |
|---|---|
| `complexity` | `file.complexity_score` |
| `max_cc` / `avg_cc` | `file.max/avg_cyclomatic_complexity` |
| `max_depth` / `avg_depth` | `file.max/avg_block_depth` |
| `logical_loc` | `file.logical_loc` |

### 의미 피처

| 컬럼 | 출처 |
|---|---|
| `bm25` | `retrieval` 산출 |
| `bm25_rank` | 〃 |
| `embed` | 〃 — Qwen3-Embedding-8B 청크 maxsim |
| `embed_rank` | 〃 — 인스턴스 내 내림차순, 동점은 비관적 |

### 메타 (피처로 쓰지 않음)

| 컬럼 | 용도 |
|---|---|
| `is_generated` | 후보 필터링용. **피처로 쓰면 안 됨** |
| `commit_skew` | repo당 스냅샷 고정 시, 스냅샷과 `base_commit`의 커밋 거리 |

---

## 2. 지표 계약

`bench/metrics/ranking.py`가 **유일한 구현**이다. 구현이 두 개면 숫자가 갈린다.

```python
from bench.metrics.ranking import recall_at_k, mrr, ndcg_at_k, reachable_recall_ceiling
```

- **동점 처리는 비관적(최악 순위)** — 낙관적 처리는 점수를 부풀린다.
- `reachable_recall_ceiling`을 항상 함께 보고한다. gold 파일이 스캔 결과에 없으면
  Recall 상한이 1 미만인데, 모르면 낮은 점수를 알고리즘 탓으로 오해한다.

---

## 3. 무결성 규칙 (전 에이전트 공통)

1. train/test 분할은 **`instance_id` 단위.** 파일 단위 분할 금지
2. `is_positive == 0`을 negative로 쓰지 않는다
3. 피처는 **`base_commit` 시점 코드에서만** 나온다. 패치 적용 후 스캔 금지
4. 하이퍼파라미터는 **train 내부 CV로만** 고른다. test를 보고 고르면 그 숫자는 폐기
5. `bench/bin/`의 **pin된 바이너리만** 쓴다. `build/`를 직접 호출하지 않는다
6. 인스턴스 DB는 **피처 추출 직후 삭제**한다 (680파일 = 24MB, 여유 공간 33GB)
7. 실패한 인스턴스를 조용히 건너뛰지 않는다. 성공/실패 개수를 반드시 보고

---

## 4. 파이프라인

```
git checkout <base_commit>
  → bench/bin/TelescodeScanner <repo> bench/scratch/<id>.db
  → bench/bin/TelescodeAlgo bench/scratch/<id>.db
  → bench/features/extract.py  →  bench/data/features.csv
  → rm -f bench/scratch/<id>.db
```

---

## 5. 진행 상태

> 결과 수치는 **`bench/RESULTS.md`** 참조. 이 표는 작업 상태만 추적한다.

| Phase | 담당 | 상태 |
|---|---|---|
| 0 — baseline 수정 | `algo-core` | ✅ 완료 (`9a05af6`, `f690a1c`) |
| 1 — 하네스 | `bench-harness` | ✅ 완료. 229 인스턴스, ceiling 0.987 |
| 2 — BM25 seed | `retrieval` | ✅ 완료. 누출 검증(`full`/`no_trace`/`no_paths`) 포함 |
| 2b — 임베딩 seed | `retrieval` | ✅ 완료. Qwen3-Embedding-8B, 229/229, $0.97 |
| 3 — PPR | `algo-core` | ✅ 완료. embed seed로 `topk1_pow1` 선택, 229개 투입 |
| 4 — LTR + ablation | `ltr-eval` | ✅ 완료. 유의성 검정 포함 |
| 상시 — 감사 | `eval-auditor` | 2라운드 완료. 1차 BLOCK 3건 해소 → 2차 WARN |

### 실험 결론

**주 가설은 기각됐다.** "구조 신호가 의미 신호와 독립적으로 기여한다"를 보이려
임베딩 seed를 도입했으나, graph 그룹의 한계 기여는 오히려 더 약해졌다
(−0.020 p=0.074 → +0.006 p=0.743). 상세는 `bench/RESULTS.md` §6.

부수적으로 시스템 성능은 크게 올랐다 (LTR MRR 0.594 → 0.730).

`non_overlap` 구간은 **확정됐다.** 구간이 작았던 원인은 repo 부족이 아니라
`vocab_overlap.csv`가 xarray만 덮고 pytest 119개를 빠뜨린 것이었다. 라벨을
마저 채우자 17 → 40 인스턴스가 되어 노이즈 기준을 넘었다 (RESULTS.md §7).

### 남은 것

1. seed와 무관한 구조 피처 시험 (RESULTS.md §11) — 기각된 가설의 재도전 경로
2. bm25 재조인을 커밋된 경로로 (감사 S2-2)
3. `bench/features/test_extract.py` — C++/Python `resolve_module` 동등성 가드 (감사 S2-3)
4. 읽기 방향 제품 결정 (`docs/reading-direction.md`)

---

## 6. Python 환경 (필수)

시스템 python3는 **PEP 668로 잠겨 있다** (`pip install` 실패). 전용 venv를 쓴다.

```bash
bench/.venv/bin/python -m pytest bench/metrics/test_ranking.py -q
bench/.venv/bin/python bench/collect/...
bench/.venv/bin/pip install <pkg>        # 추가 패키지는 여기에만
```

- `--system-site-packages`로 만들어 numpy / pandas / sklearn / torch를 상속한다 (torch 재설치 없음)
- 이미 설치됨: `pytest`, `datasets`
- **`python3`(시스템)로 벤치 스크립트를 돌리지 마라.** `datasets` import가 실패한다
- `bench/.venv/`는 gitignore 대상
