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

| Phase | 담당 | 상태 |
|---|---|---|
| 0 — baseline 수정 | `algo-core` | 대기 |
| 1 — 하네스 | `bench-harness` | Phase 0 이후 |
| 2 — BM25 seed | `retrieval` | 스텁으로 선행 개발 가능 |
| 3 — PPR | `algo-core` | Phase 0 이후 |
| 4 — LTR + ablation | `ltr-eval` | Phase 1·2 이후 |
| 상시 — 감사 | `eval-auditor` | Wave 2부터 |

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
