# Issue #37 — Issue-conditioned Ranking 실험 결과

> 측정일 2026-08-24 · baseline 커밋 `e2501cb` · 바이너리 `bench/bin/PINNED_FROM.txt`
> 원시 산출물은 `bench/data/*.csv` (gitignore 대상 — 재생성 방법은 §7)

---

## 1. 한 줄 요약

**Hybrid Learning-to-Rank가 BM25 단독 대비 MRR +0.129 (95% CI [0.088, 0.166], p < 0.001), deterministic baseline(PageRank+Betweenness) 대비 +0.461.**

단, **개선의 대부분은 semantic 신호에서 나온다.** 구조(그래프) 신호의 독립 기여는 +0.015로 작고, 어휘 비중첩 구간에서만 뚜렷하다.

---

## 2. 실험 설계

| 항목 | 내용 |
|---|---|
| 과제 | GitHub Issue 텍스트 → 수정될 파일 랭킹 (file-level localization) |
| 데이터 | SWE-bench Python — `pydata/xarray` 110 + `pytest-dev/pytest` 119 = **229 인스턴스**, gold 437 파일 |
| 라벨 | merged PR의 gold patch가 수정한 파일. **positive-unlabeled** (미수정 파일을 negative로 쓰지 않음) |
| 분할 | `instance_id` 단위 (sha256 안정 해시). train 80 / test 30, CV fold는 train 내부에만 |
| 지표 | Recall@K, MRR, NDCG@K. 동점은 **비관적 처리**(gold를 뒤로), 분모는 gold 전체 |
| `reachable_recall_ceiling` | **0.987** — gold의 98.7%가 스캔에 존재. 라벨 품질 양호 |

### 조건 두 가지

이슈 텍스트의 스택 트레이스가 gold 파일 **경로를 그대로 포함**하는 경우가 있다. 그대로 두면 BM25가 부당하게 강해 보인다.

- `full` — 원문 그대로
- `no_paths` — 경로 토큰 제거 ← **아래 수치는 전부 이쪽. 방어 가능한 조건이다.**

---

## 3. Baseline (229 인스턴스, `no_paths`)

| 방법 | MRR | R@10 | NDCG@10 |
|---|---|---|---|
| **LTR full (제안)** | **0.589** | **0.754** | **0.586** |
| BM25 단독 | 0.459 | 0.633 | 0.459 |
| complexity 단독 | 0.263 | 0.362 | 0.231 |
| out_deg | 0.222 | 0.325 | 0.196 |
| bc | 0.158 | 0.294 | 0.159 |
| logical_loc | 0.156 | 0.154 | 0.104 |
| B2 = 0.6·PR + 0.4·BC | 0.128 | 0.294 | 0.144 |
| pagerank | 0.116 | 0.213 | 0.108 |
| in_deg | 0.100 | 0.189 | 0.093 |
| `combined` (제품 현재 설정) | 0.098 | 0.181 | 0.084 |
| `file_rank` (**제품 현재 출력**) | **0.054** | 0.048 | 0.021 |

### 유의성 (부트스트랩, n_paired = 229)

| 비교 | 차이 | 95% CI | p |
|---|---|---|---|
| LTR − BM25 | **+0.129** | [+0.088, +0.166] | < 0.001 |
| LTR − complexity | **+0.325** | [+0.266, +0.384] | < 0.001 |
| LTR − B2 | **+0.461** | — | < 0.001 |

CI 하한이 전부 0을 넘는다.

---

## 4. Ablation — 어떤 신호가 실제로 기여했는가

| 구성 | 전체 (229) | overlap (106) | non_overlap (17) |
|---|---|---|---|
| LTR full | **0.589** | 0.583 | **0.201** |
| LTR −graph | 0.574 (**−0.015**) | 0.577 | 0.186 (−0.015) |
| LTR −complexity | 0.577 (−0.012) | 0.578 | 0.168 (−0.033) |
| LTR −semantic | 0.269 (**−0.320**) | 0.377 | 0.099 (−0.102) |
| graph only | 0.272 | 0.394 | 0.105 |
| complexity only | 0.288 | 0.390 | 0.111 |
| semantic only | 0.456 | 0.434 | 0.108 |

MRR 기준.

**semantic이 지배적이다.** 빼면 0.589 → 0.269로 반토막 난다. graph와 complexity의 독립 기여는 각각 −0.015, −0.012로 작다.

---

## 5. 핵심 발견 — 구간에 따라 우열이 뒤집힌다

`non_overlap` = 이슈 텍스트에 그 파일의 식별자가 **한 번도 등장하지 않는** gold 파일 구간. BM25가 원리상 찾을 수 없는 곳이다.

| | 전체 | non_overlap |
|---|---|---|
| BM25 | **0.459** | 0.102 (**4.5배 붕괴**) |
| complexity | 0.263 | 0.118 |
| semantic only (LTR) | 0.456 | 0.108 |
| **graph only (LTR)** | 0.272 | **0.105** |
| **`combined` (제품 점수)** | 0.098 | **0.291** ※ |

※ xarray 110 기준 별도 측정. 통합 매트릭스 재측정 필요 (§6).

**검색은 어휘가 겹칠 때 강하고, 겹치지 않으면 무너진다.** 그 구간에서 구조 신호가 상대적으로 유효해진다.

> 근거 사례: 로그인 버그를 고치려면 `auth/login.py`가 상속하는 세션 기반 클래스를 읽어야 하는데, 그 파일에 "login"이 한 번도 안 나올 수 있다. 임베딩·BM25는 못 찾고 그래프는 찾는다.

---

## 6. 한계 — 결론에 쓰기 전 반드시 밝힐 것

1. **`non_overlap` 구간이 작다.** 17 인스턴스 / gold 51개. 사전에 정한 노이즈 기준(20 인스턴스) 아래다. 이 구간 수치는 **유망한 신호이지 검증된 결론이 아니다.**

2. **구간 정의에 민감하다.** `vocab_definition_sweep.csv` 참조 — 정의 파라미터(`module_names`, `min_tokens`, `max_df_ratio`)에 따라 `non_overlap` 크기가 gold **2개(0.8%)~191개(73.5%)**까지 움직인다. 즉 정의를 조정해 유리한 결과를 만들 수 있다. 현재 정의를 고정하고 그 근거를 함께 보고해야 한다.

3. **라벨 편향.** "수정된 파일" ≠ "이해하기 위해 읽어야 할 파일". gold는 정답의 부분집합이고 맥락 파일을 과소평가한다 (Kochhar et al., ASE 2014). positive-unlabeled로 다뤘으나 편향 자체는 남는다.

4. **`complexity`는 부분적으로 크기 대리변수다.** `logical_loc` 단독이 0.156인 반면 gold complexity 평균 0.775 vs 비gold 0.453이라 순수 크기 효과는 아니지만, 크기 통제 비교를 별도로 봐야 한다 (`ltr229_size_control.csv`).

5. **repo 2개뿐이다.** xarray, pytest. 일반화 주장은 불가.

---

## 7. 제품에 대한 함의

**`file_rank`(제품이 실제로 출력하는 읽기 순서)가 모든 구간에서 최하위다** — 전체 0.054로 원시 `pagerank`(0.116)의 절반도 안 된다.

원인은 위상정렬 방향이다. condensation 간선을 뒤집어 leaf-first가 되는데(`ReadingSequencer.cpp`), 이는 "패치되는 파일"과 구조적으로 반대 방향이다. `docs/reading-direction.md` 참조.

⚠ **단 SWE-bench는 *관련성*을 재는 벤치마크이지 *읽는 순서*를 재지 않는다.** 온보딩 목적이라면 leaf-first가 옳을 수 있다. 벤치마크 점수를 근거로 뒤집을 사안이 아니며, 제품 결정으로 남아 있다.

---

## 8. 재현 방법

```bash
# 환경 (시스템 python3는 PEP 668로 잠김)
bench/.venv/bin/python -m pytest bench/ -q

# 바이너리는 bench/bin/ 의 pin된 것만 사용 (PINNED_FROM.txt 에 SHA·md5)
# 파이프라인: git archive <base_commit> → TelescodeScanner → TelescodeAlgo
#             → bench/features/extract.py → bench/data/*.csv → scratch DB 삭제
```

산출물 대응:

| 파일 | 내용 |
|---|---|
| `ltr229_nopaths_baselines.csv` | §3 baseline 표 |
| `ltr229_nopaths_ablation.csv` | §4 ablation |
| `ltr229_nopaths_tests.json` | §3 유의성 검정 |
| `vocab_segment_eval.csv` | §5 구간별 BM25 |
| `vocab_definition_sweep.csv` | §6-2 정의 민감도 |
| `ltr229_size_control.csv` | §6-4 크기 통제 |

---

## 9. 미해결

- `non_overlap` 구간 확대 (repo 추가) — 현재 17 인스턴스로는 §5 결론을 확정할 수 없다
- 읽기 방향 bottom-up vs top-down 제품 결정 (§7)
- 온보딩용 순서와 작업 지원용 순서를 한 모델로 낼지, 모드를 분리할지
- Personalized PageRank(`TelescodePPR`)는 구현·테스트 완료됐으나 **아직 LTR 피처로 투입되지 않았다**
