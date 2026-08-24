# Issue #37 — Issue-conditioned Ranking 실험 결과

> 측정일 2026-08-24 · baseline 커밋 `e2501cb` · 바이너리 md5는 `bench/bin/PINNED_FROM.txt`
> 원시 산출물은 `bench/data/*.csv` (gitignore 대상 — 재생성 방법은 §8)

---

## 1. 한 줄 요약

**Hybrid Learning-to-Rank가 deterministic baseline(PageRank+Betweenness) 대비 MRR +0.466, BM25 단독 대비 +0.135. 전부 p < 0.001, n = 229.**

그리고 이 프로젝트의 핵심 주장이 별도로 확인됐다 — **같은 그래프라도 목적 조건부로 전파하면(PPR) 전역 중요도(PageRank) 대비 MRR +0.259**로 3.2배 차이가 난다.

단, ablation 기준으로는 **semantic 신호만이 단독으로 필수적**이다 (§5).

---

## 2. 실험 설계

| 항목 | 내용 |
|---|---|
| 과제 | GitHub Issue 텍스트 → 수정될 파일 랭킹 (file-level localization) |
| 데이터 | SWE-bench Python — `pydata/xarray` 110 + `pytest-dev/pytest` 119 = **229 인스턴스**, gold 437 파일 |
| 라벨 | merged PR의 gold patch가 수정한 파일. **positive-unlabeled** (미수정 파일을 negative로 쓰지 않음) |
| 분할 | `instance_id` 단위 (sha256 안정 해시). `cv_fold` 0~4 = train, −1 = test |
| 지표 | Recall@K, MRR, NDCG@K. 동점은 **비관적 처리**(gold를 뒤로), 분모는 gold 전체 |
| 유의성 | 부트스트랩 95% CI, paired |
| `reachable_recall_ceiling` | **0.987** — gold의 98.7%가 스캔에 존재 |

### 조건

이슈 텍스트의 스택 트레이스가 gold 파일 **경로를 그대로 포함**하는 경우가 있다. 그대로 두면 BM25가 부당하게 강해 보인다.

- `full` — 원문 그대로
- **`no_paths`** — 경로 토큰 제거 ← **아래 수치는 전부 이쪽. 방어 가능한 조건이다.**

---

## 3. 단일 피처 baseline (229, `no_paths`)

| 피처 | MRR | R@10 | NDCG@10 | 성격 |
|---|---|---|---|---|
| bm25 | **0.459** | 0.633 | 0.459 | 의미 |
| **ppr (Phase 3)** | **0.375** | 0.432 | 0.332 | **목적 조건부 구조** |
| complexity | 0.263 | 0.362 | 0.231 | 크기·복잡도 |
| out_deg | 0.222 | 0.325 | 0.196 | 전역 구조 |
| bc | 0.158 | 0.294 | 0.159 | 전역 구조 |
| logical_loc | 0.156 | 0.154 | 0.104 | 크기 |
| B2 = 0.6·PR + 0.4·BC | 0.128 | 0.294 | 0.144 | 전역 구조 |
| pagerank | 0.116 | 0.213 | 0.108 | 전역 구조 |
| in_deg | 0.100 | 0.189 | 0.093 | 전역 구조 |
| `combined` (제품 현재 설정) | 0.098 | 0.181 | 0.084 | — |
| `file_rank` (**제품 현재 출력**) | **0.054** | 0.048 | 0.021 | — |

---

## 4. 핵심 결과 — 목적 조건부 전파 vs 전역 중요도

**같은 그래프에서 나온 두 신호의 차이다.** PPR은 BM25 seed에서 출발한 개인화 전파, 나머지는 목적과 무관한 전역 지표다.

| 비교 | 차이 (MRR) | 95% CI | p |
|---|---|---|---|
| **ppr − pagerank** | **+0.259** | [+0.206, +0.316] | < 0.001 |
| **ppr − B2 (PR+BC)** | **+0.247** | [+0.196, +0.304] | < 0.001 |
| ppr − bc | +0.217 | [+0.163, +0.276] | < 0.001 |
| ppr − complexity | +0.111 | [+0.051, +0.175] | < 0.001 |
| ppr − bm25 | **−0.085** | [−0.107, −0.061] | < 0.001 |

LTR 내부에서 graph 그룹만 떼어 봐도 같은 결론이다:

```
LTR graph global only (ppr 제외)   MRR 0.272
LTR graph only        (ppr 포함)   MRR 0.455      +0.183
```

**"코드베이스 전체에서 중요한 파일"과 "지금 목적에 중요한 파일"은 다르다** — 이 프로젝트가 문제를 재정의한 근거가 수치로 확인됐다.

### PPR이 작동하려면 seed를 뾰족하게 해야 한다

BM25는 후보 152개 **전부**에 양수 점수를 준다. 유효 지지 크기(exp(entropy))가 113.2로 거의 균등하다. `d = 0.85`에서 teleport 기여는 15%인데 그게 균등하면 **PPR이 plain PageRank로 수렴해 개인화가 씻겨 나간다.**

`sharpen_seed`(top-k 절단 + 거듭제곱)로 해결했다. 변환별 유효 지지 크기(중앙값, 원래 152):

```
없음 113.2 | power=2 70.9 | power=3 42.1 | top-20 19.5 | top-10 9.9
```

`topk`/`power`는 하이퍼파라미터이므로 **train fold 0~4로만 선택**했다. `ppr_select.py:48-52`가 test(`cv_fold < 0`)가 섞이면 예외를 던져 코드로 강제한다.

결과적으로 `corr(ppr, pagerank)` = xarray 0.251 / pytest 0.615 — 중복이 아니다.

---

## 5. Hybrid LTR과 Ablation

### 최종 성능

`LTR full` — **MRR 0.594, R@10 0.774, NDCG@10 0.597**

| 비교 | 차이 (MRR) | 95% CI | p |
|---|---|---|---|
| LTR − B2 (deterministic baseline) | **+0.466** | [+0.410, +0.522] | < 0.001 |
| LTR − complexity | +0.331 | [+0.271, +0.389] | < 0.001 |
| LTR − ppr | +0.219 | [+0.168, +0.267] | < 0.001 |
| LTR − bm25 | **+0.135** | [+0.097, +0.171] | < 0.001 |

### Ablation (229, `no_paths`, MRR)

| 구성 | MRR | full 대비 | 95% CI | p |
|---|---|---|---|---|
| **LTR full** | **0.594** | — | — | — |
| LTR −complexity | 0.578 | −0.016 | [−0.041, +0.011] | 0.260 |
| LTR −graph | 0.574 | −0.020 | [−0.045, +0.003] | 0.074 |
| LTR −ppr | 0.589 | −0.005 | — | — |
| **LTR −semantic** | **0.471** | **−0.123** | [−0.166, −0.081] | **< 0.001** |
| graph only | 0.455 | −0.139 | [−0.183, −0.094] | < 0.001 |
| semantic only | 0.456 | −0.138 | [−0.174, −0.100] | < 0.001 |
| complexity only | 0.288 | −0.306 | [−0.361, −0.253] | < 0.001 |
| graph global only (ppr 제외) | 0.272 | — | — | — |

**정직하게: 개별 그룹 제거는 semantic만 유의하다.** graph(p=0.074)와 complexity(p=0.260)는 CI가 0을 포함한다. 서로 정보가 겹쳐 하나를 빼도 나머지가 메운다.

이는 §4와 모순되지 않는다. **PPR은 단독으로는 전역 지표를 압도하지만(+0.259), BM25가 이미 있는 상태에서의 한계 기여는 작다.** PPR seed가 BM25에서 나오므로 중복은 구조적이다.

---

## 6. 어휘 비중첩 구간 — 미확정

`non_overlap` = 이슈 텍스트에 그 파일의 식별자가 **한 번도 등장하지 않는** gold 파일 구간. BM25가 원리상 찾을 수 없는 곳이다.

| | 전체 | non_overlap |
|---|---|---|
| BM25 | 0.459 | **0.102** (4.5배 붕괴) |
| complexity | 0.263 | 0.118 |

**BM25가 어휘 비중첩 구간에서 붕괴하는 것은 확실하다.** 그러나 그 자리를 구조가 메우는지는 **확정하지 못했다** — 이 구간이 16~17 인스턴스 / gold 49~51개로, 사전에 정한 노이즈 기준(20 인스턴스) 아래다. LTR ablation의 구간별 수치도 부호가 일정하지 않다.

**결론에 쓰지 말 것.** repo를 추가해 구간을 키운 뒤 재측정해야 한다.

---

## 7. 한계

1. **`non_overlap` 구간이 작다** (§6). 17 인스턴스로 노이즈 기준 미달.
2. **구간 정의에 민감하다.** `vocab_definition_sweep.csv` — 정의 파라미터에 따라 크기가 gold **2개(0.8%)~191개(73.5%)**까지 움직인다. 정의를 고정하고 근거를 함께 보고해야 한다.
3. **라벨 편향.** "수정된 파일" ≠ "이해하기 위해 읽어야 할 파일". gold는 정답의 부분집합이고 맥락 파일을 과소평가한다 (Kochhar et al., ASE 2014).
4. **`complexity`는 부분적으로 크기 대리변수다.** gold complexity 평균 0.775 vs 비gold 0.453이라 순수 크기 효과는 아니나, 크기 통제 비교를 별도로 봐야 한다 (`ltr_ppr_nopaths_size_control.csv`).
5. **repo 2개뿐이다** (xarray, pytest). 일반화 주장 불가.
6. **PPR seed가 BM25에서 나온다.** 구조 신호가 의미 신호와 독립이 아니므로 §5의 한계 기여가 작게 나오는 것은 구조적이다.

---

## 8. 제품에 대한 함의

**`file_rank`(제품이 실제 출력하는 읽기 순서)가 모든 지표에서 최하위다** — MRR 0.054로 원시 `pagerank`(0.116)의 절반도 안 된다.

원인은 위상정렬 방향이다. condensation 간선을 뒤집어 leaf-first가 되는데(`ReadingSequencer.cpp`), 이는 "패치되는 파일"과 구조적으로 반대다. `docs/reading-direction.md` 참조.

⚠ **단 SWE-bench는 *관련성*을 재는 벤치마크이지 *읽는 순서*를 재지 않는다.** 온보딩 목적이라면 leaf-first가 옳을 수 있다. 벤치마크 점수를 근거로 뒤집을 사안이 아니며 **제품 결정으로 남아 있다.**

---

## 9. 재현 방법

```bash
bench/.venv/bin/python -m pytest bench/ -q      # 시스템 python3는 PEP 668로 잠김

# 파이프라인: git archive <base_commit> → TelescodeScanner → TelescodeAlgo
#             → TelescodePPR(seed) → extract.py → join_bm25.py → scratch DB 삭제
# 바이너리는 bench/bin/ 의 pin된 것만 사용 (PINNED_FROM.txt 에 SHA·md5)
```

| 파일 | 내용 |
|---|---|
| `ltr_ppr_nopaths_baselines.csv` | §3 |
| `ltr_ppr_nopaths_ablation.csv` | §5 |
| `ltr_ppr_nopaths_tests.json` | §4·§5 유의성 |
| `ppr_sweep.csv` / `ppr_select.py` | §4 seed sharpening 선택 |
| `vocab_segment_eval.csv` | §6 |
| `vocab_definition_sweep.csv` | §7-2 |
| `ltr_ppr_nopaths_size_control.csv` | §7-4 |

---

## 10. 미해결

- **`non_overlap` 구간 확대** — repo 추가. §6 확정에 필요한 유일한 작업
- **읽기 방향 제품 결정** (§8) — `docs/reading-direction.md`
- 온보딩용 순서와 작업 지원용 순서를 한 모델로 낼지, 모드를 분리할지
- PPR seed를 BM25가 아닌 것에서 뽑아 §7-6의 독립성 문제를 완화할 수 있는지
- UI 통합 — `AlgoRunner::run(dbPath, cfg)`에 목적 질의 인자가 없다. 시그니처 변경 필요
