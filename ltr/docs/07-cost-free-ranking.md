# 유료 API 없는 추천 — LambdaMART 비교, 코드 어휘 피처, 어휘 비중첩 이슈 분석

> Issue #37 후속. 브랜치 `feat/37-advanced-recommendation-algorithm`, 기준 커밋 `52aa99d`.
> 전제: **임베딩·리랭커(외부 API)는 비용 때문에 쓰지 않는다.**
> 수치는 전부 SWE-bench 229 인스턴스(xarray 110 + pytest 119), 쿼리 조건 `no_paths`,
> 5-fold nested CV OOF, 짝지은 부트스트랩 95% CI다. 세부 정의는 `bench/RESULTS.md` 참조.

---

## 1. 임베딩·리랭커를 빼면 LambdaMART는 얼마인가

기존 초판 산출물 `bench/data/ltr_ppr_nopaths_*`가 정확히 이 조건이다
(피처: graph + complexity + bm25, PPR seed도 BM25).

| 구성 | MRR | R@1 | R@5 | R@10 |
|---|---|---|---|---|
| graph + complexity + bm25 | **0.594** | 0.345 | 0.651 | 0.774 |
| + embed | 0.730 | — | — | — |
| + embed + rerank | 0.753 | 0.492 | 0.794 | 0.885 |

- 향상분 대부분은 임베딩(+0.136)이고 리랭커는 +0.023.
- `LTR −semantic`(0.560)은 이 질문의 답이 **아니다** — `SEMANTIC_FEATURES`에 bm25도 들어 있다.
- 현재 매트릭스(`features_all_embedppr.csv`)에서 embed/rerank 컬럼만 빼면 PPR seed가
  임베딩에서 나와 의미 정보가 새므로 깔끔한 비교가 아니다.

## 2. 기존 추천 알고리즘 vs LambdaMART (유료 피처 없음)

| 방식 | MRR | R@1 | R@5 | R@10 | NDCG@10 |
|---|---|---|---|---|---|
| `file_rank` (제품 현재 출력) | 0.054 | 0.000 | 0.013 | 0.048 | 0.021 |
| `combined` (제품 현재 점수) | 0.098 | 0.004 | 0.045 | 0.181 | 0.084 |
| B2 = 0.6·PR + 0.4·BC | 0.128 | 0.013 | 0.125 | 0.294 | 0.144 |
| PPR 단독 | 0.375 | 0.211 | 0.320 | 0.432 | 0.332 |
| BM25 단독 | 0.459 | 0.227 | 0.502 | 0.633 | 0.459 |
| **LambdaMART** | **0.594** | **0.345** | **0.651** | **0.774** | **0.597** |

- LTR − B2 **+0.466** [+0.410, +0.522] · LTR − BM25 **+0.135** [+0.097, +0.171] · LTR − PPR +0.219 — 모두 p<0.001
- holdout(53) MRR 0.614
- **공정성 주의**: 기존 알고리즘은 이슈 텍스트를 보지 않는 전역 중요도다. 차이의 대부분은
  "이슈를 보느냐"에서 온다. 제품에 넣으려면 목적 텍스트 입력이 필요하다
  (`AlgoRunner::run`에 질의 인자 없음).

## 3. PageRank·BC에 결정론적으로 이슈를 반영할 수 있나

이미 PPR(`src/algo/AlgoRunner.h:99`)이 그 방식이다. 측정 결과:

| 방식 | MRR | R@10 |
|---|---|---|
| PPR (BM25 top-1 teleport) | 0.375 | 0.432 |
| RRF(BM25, BC) | 0.328 | 0.601 |
| RRF(BM25, PPR) | 0.454 | 0.653 |
| BM25 단독 | 0.459 | 0.633 |

- PPR은 PageRank와 순위상 거의 같다(인스턴스별 스피어만 0.94). "seed 파일 하나를 맨 앞으로"에 가깝다.
- BC는 섞을수록 나빠진다. seed를 넓히면 PPR MRR이 0.39 → 0.23 → 0.16으로 하락.
- 결론: 결정론적 이슈 반영의 성능은 대부분 BM25에서 나오고 그래프는 보조다.

## 4. 코드 어휘 피처 3종 추가 실험

### 피처 정의 (`bench/features/code_lexical.py`)

| 피처 | 정의 |
|---|---|
| `def_match` | 이슈 식별자 중 그 파일이 `def`/`class`로 **정의**하는 것의 Σ idf |
| `str_match` | 파일의 한 줄 문자열 리터럴(포맷 치환부에서 분할, docstring 제외) 조각 중 쿼리에 부분문자열로 등장하는 것의 Σ idf |
| `nbr_bm25_max` / `_mean` | import·호출 이웃(무방향) 파일들의 BM25 최댓값·평균 |

- 쿼리는 BM25 seed와 같은 `no_paths`. 코드는 `base_commit` blob에서만 읽음. 라벨 미사용.
- 임계값(식별자 3자, 리터럴 조각 12자·2단어)은 사전 고정. 샘플을 본 뒤 바꾸지 않았다.
- 간선은 `bench/features/edges.py`로 스캐너만 재실행해 추출(229/229 성공).
  기존 매트릭스의 `in_deg`/`out_deg`·파일 집합과 **전 인스턴스 정확히 일치** 검증.
- gold에서 >0 비율 (**`no_paths` 쿼리 기준**, gold 423개 = 스캔에 있는 gold):
  `def_match` 58.9% (249/423, 비gold 15.9%), `str_match` 17.0% (72/423, 비gold 2.7%).
  참고로 원문(`full`) 쿼리에서는 60.0% / 16.8%, 17.5% / 2.8% — 경로 제거의 영향은 1%p 안팎이다.

### 결과 (`bench/data/ltr_codelex_*`)

| 구성 | MRR | R@1 | R@5 | R@10 | NDCG@10 |
|---|---|---|---|---|---|
| base (기존 무료 구성, 0.5940 정확히 재현) | 0.594 | 0.345 | 0.651 | 0.774 | 0.597 |
| **full (+3종)** | **0.649** | **0.410** | **0.666** | **0.785** | **0.640** |

- full − base **+0.055** [+0.023, +0.089], p<0.001. holdout(53) 0.712.
- 임베딩 LTR(0.730)과의 격차의 약 40%를 비용 0으로 메움.
- 개선은 상위권에 집중(R@1 +0.065, R@10 +0.011).

### 피처별

| 피처 | base +X | full −X | 판단 |
|---|---|---|---|
| `def_match` | +0.026 (p=0.064) | **−0.057 (p=0.001)** | 핵심 |
| `str_match` | −0.000 (p=0.97) | −0.020 (p=0.071) | 단독 무효, 조합 시 보조 |
| `nbr_bm25` | −0.015 (p=0.14) | −0.002 (p=0.86) | **기여 없음 — 제거 가능**(−nbr 0.646) |

- 단독 추가분 합(≈+0.01)보다 full(+0.055)이 크다. 상호작용인지 튜닝 변동인지 단일 실행으로는 구분 불가.
- 이 구성에서 complexity 그룹이 유의해졌다(−complexity −0.048, p=0.005).
- 피처 gain: bm25_rank 29% · bm25 23% · def_match 9.5% · complexity 5.2%.

### ⚠ 어휘 비중첩 구간은 악화

| 구성 | overlap (214) | non_overlap (40) |
|---|---|---|
| base | 0.610 | **0.218** |
| full | 0.677 | **0.161** (−0.057, p<0.001) |

세 피처 모두 어휘 신호라 모델이 어휘에 더 기대고, 어휘가 안 맞는 이슈에서 손해를 본다.
평균이 오른 대가다.

## 5. 임베딩을 일부 이슈에만 쓰는 방안 — 효과 없음

라벨 없는 게이트(`max_def`, `bm25_max`)로 "어휘가 약한 이슈"만 골라 임베딩하는 안을 검토했다.

| 게이트가 맞히려는 대상 | AUC |
|---|---|
| non_overlap gold를 가진 이슈 (40) | 0.74 |
| 임베딩이 BM25보다 나은 이슈 (109) | **0.49** |

임베딩이 이기는 이슈 대부분은 어휘가 **겹치는** 이슈다. 단일 피처 기준 비용 곡선:

| 임베딩 적용 비율 | 게이트 | 무작위 | oracle |
|---|---|---|---|
| 0% | 0.459 | 0.459 | 0.459 |
| 20% | 0.482 | 0.472 | 0.600 |
| 50% | 0.498 | 0.491 | 0.653 |
| 100% | 0.523 | 0.523 | 0.653 |

- 게이트 ≈ 무작위. oracle 여지는 크지만 지금 신호로는 못 찾는다.
- ⚠ LTR이 아닌 단일 피처(embed vs bm25) 비교다. LTR 게이트 시뮬레이션은 미실행.
- **비용 구조**: 임베딩 $0.97은 저장소 문서 인덱싱 **1회**(캐시 가능)이고 질의 임베딩은
  거의 0이다. 이슈 단위 게이팅이 줄이는 것은 주로 리랭커(질의당 ≈$0.005)다.
- 임베딩 자체는 유효하다(LTR 0.730 vs 무료 0.649). 무의미한 것은 "어휘 약한 이슈만" 게이팅이다.

## 6. 어휘가 안 맞는 이슈의 특징

정의(`bench/seed/vocab.py`): gold 파일의 식별자(경로·클래스·함수명) 중 인스턴스 내
df ≤ 10%인 토큰이 이슈에 하나도 없으면 비중첩. gold 85/423개, 이런 gold를 가진 이슈 40개.

| 부류 | 이슈 | 설명 |
|---|---|---|
| A. 전부 비중첩 | 15 (pytest 11 · xarray 4) | 모든 gold가 비중첩 |
| B. 일부 비중첩 | 25 | 주 파일은 맞고 딸린 파일만 비중첩 |

### A. 사용자 언어로 쓴 짧은 기능 요청

| no_paths 쿼리 (중앙값) | A | 어휘 맞는 이슈 |
|---|---|---|
| 길이 | 672자 | 2,098자 |
| 코드 식별자 수 | 1 | 11 |
| 코드 블록 비율 | 40% | 84% |
| 에러명 포함 비율 | 13% | 49% |

| 이슈 표현 | 실제 수정 파일 |
|---|---|
| `NO_COLOR` 환경변수 지원 | `_io/terminalwriter.py` |
| `--collect-only` 짧은 옵션 | `main.py` |
| 실패 테스트의 임시 디렉토리만 유지 | `tmpdir.py`, `pathlib.py` |
| `_repr_html_` 기본값 전환 | `core/options.py` |
| deprecation 문서 개선 | `deprecated.py` |

사용자 개념과 구현 모듈 사이의 연결어가 없다. 일부는 정의의 사각이다 — CLI 옵션·환경변수
이름은 코드에서 식별자가 아니라 **문자열**(`addoption("--collect-only")`, `getenv("NO_COLOR")`)로만 존재한다.

### B. 주 파일에 딸려 함께 수정되는 파일

| gold 속성 (중앙값) | 비중첩 | 중첩 |
|---|---|---|
| 인스턴스 gold 수 | 4 | 2 |
| `logical_loc` | 274 | 745 |
| `in_deg` / `pagerank` | 6 / 0.006 | 9 / 0.011 |
| pagerank **인스턴스 내 백분위** | 0.88 | 0.90 |
| `__init__.py` 비율 | 10.6% | 2.7% |
| BM25 순위 | 40 | 5 |

빈출: `deprecated.py`(7), `__init__.py`(export), `pycompat.py`·`utils.py`·`duck_array_ops.py`·`types.py`, `doc/conf.py`.

### 시사점

- A → CLI 옵션·환경변수·설정 문자열 매칭. 현재 `str_match`는 12자·2단어 기준이라 이런 짧은 이름을 버린다.
- B → 동시 수정 이력(co-change, `base_commit` 이전 커밋만). `nbr_bm25`(import 관계)는 이 패턴을 못 잡았다.
- B는 라벨 편향 가능성 — export 한 줄 수정도 gold지만 "읽어야 할 파일"은 아닐 수 있다 (RESULTS.md 한계 4).
- A는 15개, 그중 11개가 pytest. 일반화 주장 불가.

> 원점수로는 비중첩 gold가 덜 중심적이지만(pagerank 0.006 vs 0.011), 인스턴스 안 순위로는
> 차이가 작다(백분위 0.88 vs 0.90). "딸린 파일 = 주변부 파일"로 읽으면 안 된다.

## 7. 결정론적 결합 공식 — BM25 × 구조 점수

`bench/ltr/weighted_sum.py`. 학습 없는 공식을 파라미터 1~2개로 흔들고, 파라미터는
**outer fold의 train 인스턴스 평균 MRR로만** 골라 test에 적용했다(fold는 LambdaMART와 동일).
그리드는 BM25와 같은 설정이 맨 앞이고 동점이면 앞쪽을 고른다.

### 전제: BM25 상위 후보 안에서 중심성이 gold를 가려내는가

| BM25 후보 | gold / 후보 | pagerank | bc | B2 | `combined` |
|---|---|---|---|---|---|
| 상위 5 | 180 / 1,145 | 0.711 | 0.662 | 0.686 | 0.689 |
| 상위 10 | 239 / 2,290 | 0.701 | 0.674 | 0.686 | 0.687 |
| 후보 전체 | 423 / 43,688 | 0.839 | 0.835 | 0.842 | 0.821 |

AUC(인스턴스를 섞어 셈). gold는 인스턴스 안 중심성 상위 10% 부근(백분위 중앙값 0.89)에 있지만
그 구간에 무관한 허브가 많아 1위를 못 한다 — 중심성은 **1위 선택 신호로는 약하고 후보 정리 신호로는 쓸 만하다.**

### 네 가지 형태

| 형태 | 점수 |
|---|---|
| `linear` | α·nb + (1−α)·np |
| `mult` | nb · (ε + np)^β |
| `gate` | (α₀ + (1−α₀)·pct) · nb |
| `cascade` | BM25 상위 N개만 w·cb + (1−w)·cp, 나머지는 BM25 순 |

(nb/np: 인스턴스 내 min-max 정규화한 bm25/구조 점수, pct: 구조 점수 백분위)

### 결과 (229, `no_paths`)

| 형태 (결합 대상) | fold별 선택 | MRR | R@1 | R@10 | non_overlap | BM25 대비 | linear(combined) 대비 |
|---|---|---|---|---|---|---|---|
| **gate (pagerank)** | α₀=0.5 (4/5), 0.25 | **0.506** | 0.264 | 0.688 | 0.199 | +0.047 p=0.001 | +0.004 p=0.58 |
| linear (combined) | α=0.7 (4/5), 0.6 | 0.502 | 0.263 | 0.691 | 0.202 | +0.043 p<0.001 | — |
| gate (B2) | α₀=0.5 (5/5) | 0.502 | 0.263 | 0.694 | 0.189 | +0.042 p<0.001 | −0.000 p=1.00 |
| mult (combined) | β 0.25~1.0 | 0.501 | 0.263 | 0.681 | 0.200 | +0.042 p<0.001 | −0.001 p=0.79 |
| cascade (combined) | fold마다 다름 | 0.492 | 0.254 | 0.689 | 0.194 | +0.033 p=0.008 | −0.010 p=0.086 |
| gate (combined) | 불안정 | 0.490 | 0.246 | 0.673 | 0.195 | +0.031 p=0.032 | −0.012 p=0.20 |
| linear (B2) | α=0.9 (5/5) | 0.473 | 0.238 | 0.644 | 0.156 | +0.014 p=0.028 | −0.029 p=0.012 |
| linear (pagerank) | α=0.9 (5/5) | 0.473 | 0.238 | 0.644 | 0.154 | +0.013 p=0.003 | −0.029 |
| mult / cascade (B2·pagerank) | β=0.25 / 불안정 | 0.470~0.473 | | | 0.16~0.19 | +0.01 (n.s.) | −0.03 |
| linear (ppr / bc) | α 0.75~0.95 | 0.462 / 0.455 | | | 0.16 / 0.15 | +0.003 / −0.004 (n.s.) | |
| BM25 단독 | — | 0.459 | 0.227 | 0.633 | 0.153 | — | |
| 참고: RRF(bm25, combined) | 반반 고정 | 0.331 | 0.068 | 0.642 | | −0.129 | |

- 모든 공식이 LambdaMART보다 유의하게 낮다(−0.09~−0.18, p<0.001). non_overlap에서는 서로 간 차이가 유의하지 않다.
- RRF(bm25, combined)가 낮았던 이유는 **비율**이었다. 7:3이면 BM25를 유의하게 넘는다.
- 덧셈(`linear`)은 무관한 허브에도 점수를 줘서 B2를 α=0.9까지 밀어냈다. **게이트**로 쓰면 B2·pagerank가
  `combined` 선형과 같은 수준까지 올라온다(linear B2 0.473 → gate B2 0.502, 직접 검정은 안 함).
- `mult`는 가장 약한 β로 수렴해 사실상 BM25, `cascade`는 fold 간 설정이 불안정했다.
- 어느 형태로 섞든 구조 신호의 몫은 **+0.04~0.05**. §6(RESULTS.md)의 "한계 기여가 작다"와 같은 방향.

### 제품 공식 후보

```
score = (0.5 + 0.5 · pct(PageRank)) · norm(BM25)
```

"BM25에 중심성에 따라 0.5~1.0배를 곱한다". 파라미터 하나, fold 간 안정, `combined` 비의존.

> ⚠ 형태 안의 파라미터는 train으로만 골랐지만, **형태·결합 대상 14개 조합 중 1등을 고른 것은
> test OOF를 본 사후 선택**이다. 상위 형태 간 차이가 모두 유의하지 않으므로
> "0.50 수준에서 동등하고 그중 가장 단순한 것"으로 읽어야 한다.

## 8. 어휘 비중첩의 원인 — "함수명이 다르다"가 아니다

정의(`bench/seed/vocab.py`)를 다시 확인했다. gold 파일마다

    | 이슈 토큰 ∩ 파일 식별자 ∩ 흔하지 않은 토큰 | = 0  →  non_overlap

- 이슈 토큰: `no_paths` 쿼리를 BM25 토크나이저로 자름(snake/camel 분해, 소문자, 불용어 제거)
- 파일 식별자: **경로 토큰 + 클래스명 + 함수명만.** 본문·문자열·주석·모듈 상수는 제외
- 흔하지 않은 토큰: 인스턴스 파일 중 **10% 이하**에 등장

같은 함수로 재계산해 기존 라벨과 **85/85 일치**를 확인한 뒤, 이슈 단어가 어디서 겹치는지 분류했다.

| 원인 | 개수 | 예 |
|---|---|---|
| ① 모듈 수준 대입 이름에서만 겹침 | 14 | `style` → `options.py`, `skipna` → `ops.py` |
| ② 본문(문자열·주석·호출하는 이름)에서만 겹침 | 59 | `map_blocks` → `dask_array_ops.py`, `open_mfdataset` → `combine.py` |
| ③ 식별자와 겹치지만 흔한 토큰(df > 10%) | 11 | `rolling` → `rolling.py` |
| ④ 어디에도 없음 | 1 | xarray-6804 `duck_array_ops.py` |

- **84/85는 이슈 단어가 그 파일 어딘가에 있다.** 비중첩은 "이슈가 그 파일을 특정하는 이름을 부르지 않는다"에 가깝다.
- ②에는 `added`, `taken`, `cases` 같은 일반 단어의 우연한 겹침이 섞여 있다. 유효 개수는 59보다 적다.
- BM25는 본문을 인덱싱하지만 이 파일들의 BM25 순위 중앙값은 40이다 — 겹치는 단어가 흔하거나 여러 파일에 나온다.

## 9. 모듈 이름·심볼 사용처 피처 — 비중첩 구간 소폭 회복, 전체 이득 없음

§8의 ①·②를 겨냥했다 (`bench/features/code_lexical.py`).

| 피처 | 정의 |
|---|---|
| `mod_match` | 이슈 식별자 중 파일이 **모듈 수준 대입 이름**으로 정의하는 것의 Σ idf |
| `sym_use` | 이슈 식별자 중 **저장소 어딘가에 정의가 있고**, 이 파일은 정의하지 않고 **코드에서 사용만** 하는 것의 Σ idf(사용 df). docstring·주석 제외 |
| `sym_prop` | `sym_use` 중 이 파일이 그 심볼의 **정의 파일과 간선으로 직접 연결**된 것만 — 이슈 심볼에서 호출 그래프 한 칸 |

### 사전 진단 (라벨은 분포 확인만, 튜닝 없음)

| 피처 | 비중첩 gold(85) >0 | 순위 중앙값 | 중첩 gold(338) >0 | 순위 중앙값 |
|---|---|---|---|---|
| `def_match` | 9.4% | 172 | 71.3% | 7 |
| `mod_match` | 2.4% | 172 | 3.0% | 170 |
| `sym_use` | 67.1% | 60 | 92.6% | 27 |
| `sym_prop` | 49.4% | 137 | 82.0% | 20 |
| BM25 | — | 40 | — | 5 |

- 비중첩 gold 중 BM25 10위 안 19개 → `sym_prop` 10위 안까지 합치면 24개.
- `mod_match`가 2.4%인 이유: §8 ①의 14개는 **토큰 조각** 단위 겹침(`DISPLAY_STYLE`의 `style`)이었는데
  `mod_match`는 `def_match`처럼 **이름 전체** 일치다. 진단을 보고 매칭 방식을 바꾸면 test를 보고
  설계를 고치는 것이라 그대로 뒀다.

### LambdaMART 결과 (`bench/data/ltr_codelex2_*`)

base = §4의 full(무료 피처 + def·str·nbr, 0.6488 정확히 재현).

| 구성 | MRR | R@1 | R@10 | overlap | **non_overlap** | non_overlap R@10 |
|---|---|---|---|---|---|---|
| base | **0.649** | 0.410 | 0.785 | 0.677 | 0.161 | 0.314 |
| base + `sym` | 0.646 | 0.396 | 0.797 | 0.671 | **0.178** | **0.414** |
| base + `mod_match` | 0.638 | 0.390 | 0.791 | 0.662 | 0.177 | 0.372 |
| full (둘 다) | 0.630 | 0.377 | 0.798 | 0.658 | 0.159 | 0.401 |

| base 대비 ΔMRR | 전체 | non_overlap |
|---|---|---|
| + `sym` | −0.003 (p=0.84) | **+0.017** [+0.001, +0.038] p=0.043 |
| + `mod_match` | −0.011 (p=0.29) | +0.016 (p=0.23) |
| full | −0.019 (p=0.079) | −0.003 (p=0.95) |

- `sym`은 비중첩 구간에 약한 신호가 있다. MRR은 경계선 유의(다중 검정 감안하면 약함), R@10은 +0.10
  (R@10 차이는 유의성 미검정). **1위로는 못 올리고 10위 안으로 끌어올린다.** 전체는 변화 없음.
- `mod_match`는 효과 없음(진단과 일치). 둘을 함께 넣으면 오히려 하락 — 새 피처는 gain 상위 8위에 없었다.
- 비중첩 구간 목표(무료 LTR 0.218, gate 0.199)에는 미달. 코드 어휘 피처로 잃은 몫(0.218 → 0.161)의 약 1/3 회복.
- 결론: "이슈 심볼에서 출발하는 구조 전파"는 **방향은 맞지만 효과가 작다.** 같은 심볼을 쓰는 파일이
  너무 많아 gold를 위로 올리지 못한다.

### 현재 무료 구성 선택지

| 목적 | 선택 | 전체 MRR | non_overlap MRR |
|---|---|---|---|
| 전체 성능 | LambdaMART + 코드 어휘 (+`sym` 선택) | 0.649 | 0.161~0.178 |
| 구간 균형 | 무료 LambdaMART | 0.594 | 0.218 |
| 단순·설명 가능 | gate 공식 | 0.506 | 0.199 |

## 10. 한계

- repo 2개, 단일 실행. 새 피처 임계값은 사전 고정이나 설계 자체는 이 데이터를 본 뒤 했다.
- §7의 형태 선택과 §9의 원인 분류는 전체 데이터를 보고 한 사후 분석이다.
- **부동소수점 비결정성(수정됨).** `code_lexical`의 점수 합산이 set 순회 순서를 따라, 문자열 해시
  무작위화로 실행마다 1e-14 수준 차이가 났다. 합산 전 정렬로 고쳤고 `PYTHONHASHSEED=1`·`2`의
  출력이 **비트 단위로 일치**함을 확인했다. §4·§9의 산출 CSV는 수정 전 실행본이며 수정본과의 최대
  차이는 4.5e-12다. §9의 base가 §4의 0.6488을 정확히 재현해 결론에 영향은 없다.
- `str_match`는 `no_paths`에서도 남는 예외 요약 줄에서 일부 신호를 얻는다(경로 누출 아님, 증상 정보).
- LambdaRank는 unlabeled(0)를 최하위로 학습한다 — 그래프 피처에 불리한 편향 (`bench/ltr/model.py` docstring).

## 11. 다음 후보

1. `nbr_bm25`를 뺀 경량 구성 확정
2. A형 대응: 옵션·환경변수 문자열 매칭 피처 (§8 ①의 토큰 조각 매칭 포함 — 새 데이터로 검증 필요)
3. B형 대응: `base_commit` 이전 co-change 피처 (누수 검증 포함)
4. non_overlap 손실 완화: LambdaMART(코드 어휘)와 gate 공식의 앙상블 — 두 모델이 강한 구간이 다르다
5. 제품 반영 검토: gate 공식 `(0.5 + 0.5·pct(PR))·norm(BM25)` — `AlgoRunner::run`에 질의 인자 필요
6. (선택) LTR 기반 임베딩 게이팅 시뮬레이션 — 기존 seed로 비용 0

## 재현

```bash
# 간선 재추출 (스캐너만, 약 4.5분)
bench/.venv/bin/python -m bench.features.edges \
  --features bench/data/features_all_with_bm25__no_paths.csv \
  --manifest bench/data/instances_all.csv \
  --add-repo pydata/xarray=bench/repos/xarray \
  --add-repo pytest-dev/pytest=bench/repos/pytest \
  --out bench/data/file_edges_all.csv

# 코드 어휘 피처 (약 15초)
bench/.venv/bin/python -m bench.features.code_lexical \
  --features bench/data/features_all_with_bm25__no_paths.csv \
  --edges bench/data/file_edges_all.csv \
  --add-repo pydata/xarray=bench/repos/xarray \
  --add-repo pytest-dev/pytest=bench/repos/pytest \
  --condition no_paths --out bench/data/code_lexical__no_paths.csv

# LTR + ablation (약 1.5시간)
bench/.venv/bin/python -m bench.ltr.run_ltr \
  --features bench/data/features_all_with_bm25__no_paths.csv \
  --manifest bench/data/instances_all.csv \
  --bm25-seed bench/data/seed_all/bm25_seed__no_paths.csv \
  --extra-features bench/data/code_lexical__no_paths.csv \
  --overlap bench/data/vocab_overlap.csv --overlap-condition no_paths \
  --splits bench/data/splits_all.csv --out-prefix bench/data/ltr_codelex

# 결정론적 결합 공식 4형태 (LambdaMART OOF는 캐시, 첫 실행 약 15분)
bench/.venv/bin/python -m bench.ltr.weighted_sum \
  --features bench/data/features_all_with_bm25__no_paths.csv \
  --manifest bench/data/instances_all.csv \
  --extra-features bench/data/code_lexical__no_paths.csv \
  --overlap bench/data/vocab_overlap.csv --overlap-condition no_paths \
  --out-prefix bench/data/wsum3_nopaths

# 모듈 이름·심볼 사용처 피처 (위 code_lexical을 --out code_lexical2__no_paths.csv로 재실행 후)
bench/.venv/bin/python -m bench.ltr.run_ltr \
  --features bench/data/features_all_with_bm25__no_paths.csv \
  --manifest bench/data/instances_all.csv \
  --bm25-seed bench/data/seed_all/bm25_seed__no_paths.csv \
  --extra-features bench/data/code_lexical2__no_paths.csv \
  --base-extra-groups def_match,str_match,nbr_bm25 --extra-only \
  --overlap bench/data/vocab_overlap.csv --overlap-condition no_paths \
  --splits bench/data/splits_all.csv --out-prefix bench/data/ltr_codelex2
```

| 파일 | 내용 |
|---|---|
| `bench/features/edges.py` | 간선 재추출 + 차수 일치 검증 |
| `bench/features/code_lexical.py` | 피처 3종 (`CX` 컬럼 상수 — `schema.py`는 계약상 미수정) |
| `bench/features/test_code_lexical.py` | 단위 테스트 |
| `bench/ltr/data.py`, `bench/ltr/run_ltr.py` | `--extra-features` 조인·ablation 구성 추가 (미지정 시 기존 동작) |
| `bench/data/file_edges_all.csv` | 간선 174,577개 |
| `bench/data/code_lexical__no_paths.csv` | 피처 매트릭스 |
| `bench/data/ltr_codelex_*` / `ltr_codelex.log` | 결과·유의성 |
| `bench/ltr/weighted_sum.py`, `test_weighted_sum.py` | 결정론적 결합 4형태 (§7) |
| `bench/data/wsum_nopaths_*` | 선형 가중합 1차 실행 |
| `bench/data/wsum3_nopaths_*` | 4형태 결과·그리드 곡선·유의성 |
| `bench/data/ltr_oof_nopaths__{base,codelex}.csv/.json` | LambdaMART OOF 캐시 (피처·fold 지문 포함) |
| `bench/data/code_lexical2__no_paths.csv` | §9 피처 포함 매트릭스 |
| `bench/data/ltr_codelex2_*` / `ltr_codelex2.log` | §9 결과·유의성 |
| `bench/ltr/run_ltr.py` | `--base-extra-groups`, `--extra-only` 추가 |
