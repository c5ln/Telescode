# Issue #37 — Issue-conditioned Ranking 실험 결과

> 측정일 2026-08-25 (임베딩 도입) / 2026-08-24 (초판) · baseline 커밋 `e2501cb`
> 바이너리 md5는 `bench/bin/PINNED_FROM.txt` · 원시 산출물은 `bench/data/*.csv` (gitignore — 재생성은 §10)

---

## 1. 한 줄 요약

**Hybrid Learning-to-Rank가 MRR 0.753. deterministic baseline(PageRank+Betweenness) 대비 +0.626, BM25 단독 대비 +0.294. 전부 p < 0.001, n = 229.**

그리고 **가설 하나가 기각됐고, 반증이 세 단계에 걸쳐 강해졌다.** 의미 신호를 강화할수록
graph 그룹의 한계 기여는 무용을 지나 **유해**가 됐다.

| 단계 | `-graph − full` | 95% CI | p | 해석 |
|---|---|---|---|---|
| BM25만 | −0.020 | [−0.045, +0.003] | 0.074 | 빼면 손해, 유의하지 않음 |
| + 임베딩 | +0.006 | [−0.023, +0.033] | 0.743 | 효과 없음 |
| **+ 리랭커** | **+0.024** | **[+0.0002, +0.049]** | **0.046** | **빼는 게 이득, 유의함** |

| 주장 | 상태 |
|---|---|
| Hybrid LTR이 deterministic baseline을 크게 이긴다 | ✅ +0.626, p<0.001 |
| 목적 조건부 전파(PPR)가 전역 중요도(PageRank)를 이긴다 | ✅ +0.281, p<0.001 — 단 §5의 단서를 함께 읽을 것 |
| 임베딩이 BM25와 상보적이다 | ✅ §4 |
| 크로스 인코더가 어휘 비중첩 구간을 메운다 | ✅ 0.263 → 0.375 (§8) |
| 어휘 비중첩 구간에서 BM25가 붕괴한다 | ✅ ×0.33, n=40 (§7) |
| **구조 신호가 의미 신호와 독립적으로 기여한다** | ❌ **기각** — 최종적으로 **음의 기여** (§6) |

---

## 2. 실험 설계

| 항목 | 내용 |
|---|---|
| 과제 | GitHub Issue 텍스트 → 수정될 파일 랭킹 (file-level localization) |
| 데이터 | SWE-bench Python — `pydata/xarray` 110 + `pytest-dev/pytest` 119 = **229 인스턴스**, gold 437 파일 |
| 라벨 | merged PR의 gold patch가 수정한 파일. **positive-unlabeled** (미수정 파일을 negative로 쓰지 않음) |
| 분할 | `instance_id` 단위 (sha256 안정 해시). `cv_fold` 0~4 = train(176), −1 = test(53) |
| 지표 | Recall@K, MRR, NDCG@K. 동점은 **비관적 처리**(gold를 뒤로), 분모는 gold 전체 |
| 유의성 | 부트스트랩 95% CI, paired |
| `reachable_recall_ceiling` | **0.987** — gold의 98.7%가 스캔에 존재 |

### 조건

이슈 텍스트의 스택 트레이스가 gold 파일 **경로를 그대로 포함**하는 경우가 있다. 그대로 두면 BM25가 부당하게 강해 보인다.

- `full` — 원문 그대로
- **`no_paths`** — 경로 토큰 제거 ← **아래 수치는 전부 이쪽. 방어 가능한 조건이다.**

### 임베딩

| 항목 | 값 |
|---|---|
| 모델 | `qwen/qwen3-embedding-8b` (OpenRouter), MRL 1024차원 |
| 청크 | 1024 토큰 / stride 768 (겹침 25%) |
| 집계 | `maxsim` = max_c cos(q, c) — 근거는 `bench/seed/embed.py` |
| 쿼리 지시문 | *"Given a GitHub issue report, retrieve the source files that must be modified to resolve it."* |
| 규모 | 고유 blob 6,308 / 청크 75,498 / 토큰 74.7M / **비용 $0.97** |

### 리랭킹 (3단계)

| 항목 | 값 |
|---|---|
| 모델 | `cohere/rerank-4-fast` (OpenRouter) |
| 후보 | 인스턴스당 `embed` 상위 **50개** (평균 후보 191개 중) |
| 문서 | 각 파일에서 쿼리와 가장 가까운 청크 1개 + **파일 경로 접두** |
| 규모 | 229 인스턴스 · 11,450쌍 · 152초 · **비용 $1.07** |

**instruction-aware 모델을 고른 것이 설계의 일부다.** "이 이슈와 비슷한 텍스트"가 아니라 "이 이슈를 고치려면 고쳐야 할 파일"을 직접 질의할 수 있고, 그게 Issue-conditioned Ranking 그 자체다.

> ⚠ **재현성 한계.** 바이너리는 md5로 핀했지만 임베딩은 외부 API다. 엔드포인트를 고정할 수 없다.
> 완화책으로 벡터 전량을 `bench/data/seed_all/embed_blobs.npz`에 캐시하고, 인코더 설정(모델·차원·청크·지시문)을 지문으로 함께 굽는다. 지문이 다른 캐시는 로드가 **거부**된다 — 서로 다른 모델의 벡터가 섞이면 코사인이 무의미해지는데 예외는 안 나기 때문이다.

---

## 3. 단일 피처 baseline (229, `no_paths`)

| 피처 | MRR | R@10 | NDCG@10 | 성격 |
|---|---|---|---|---|
| **rerank (크로스 인코더)** | **0.651** | **0.818** | **0.649** | **의미 (재채점)** |
| embed (Qwen3) | 0.523 | 0.766 | 0.544 | 의미 (임베딩) |
| bm25 | 0.459 | 0.633 | 0.459 | 의미 (어휘) |
| **ppr** | **0.397** | 0.529 | 0.378 | **목적 조건부 구조** |
| complexity | 0.263 | 0.362 | 0.231 | 크기·복잡도 |
| out_deg | 0.222 | 0.325 | 0.196 | 전역 구조 |
| bc | 0.158 | 0.294 | 0.159 | 전역 구조 |
| logical_loc | 0.156 | 0.154 | 0.104 | 크기 |
| B2 = 0.6·PR + 0.4·BC | 0.128 | 0.294 | 0.144 | 전역 구조 |
| pagerank | 0.116 | 0.213 | 0.109 | 전역 구조 |
| in_deg | 0.100 | 0.189 | 0.093 | 전역 구조 |
| `combined` (제품 현재 설정) | 0.098 | 0.181 | 0.084 | — |
| `file_rank` (**제품 현재 출력**) | **0.054** | 0.048 | 0.021 | — |

`ppr`은 embed seed 기준이다. BM25 seed였던 초판에서는 0.375였다.
`rerank`는 상위 50개 후보만 채점하므로 나머지는 NaN이고, 랭킹 지표에서 최하위로 밀린다 —
즉 위 수치는 **후보 밖을 포기한 상태의 값**이다.

---

## 4. 임베딩과 BM25는 다른 곳에서 틀린다

당초 성공 기준은 **낮은 `corr(embed, bm25)`**였다. 실측은 **스피어만 인스턴스별 중앙값 +0.658**로 높다.

**그러나 이 지표가 재는 것이 잘못됐다.** 파일 단위 점수 상관은 두 신호가 둘 다 정확하면 필연적으로 높아진다. 중요한 것은 "같은 인스턴스에서 같이 실패하는가"다.

```
인스턴스별 MRR 상관    pearson 0.334 / spearman 0.376     ← 파일 단위 0.658보다 훨씬 낮다
```

| 구간 | bm25 | embed |
|---|---|---|
| **BM25 실패 62개** (MRR<0.1) | MRR 0.045 / **R@10 0.000** | MRR **0.328** / **R@10 0.659** |
| embed 실패 31개 | MRR 0.226 | MRR 0.045 |

```
승패        embed 109승 / 73패 / 47무
oracle(둘 중 나은 쪽)  MRR 0.653   vs  embed 0.523  bm25 0.460
상보성 여지            +0.130
```

**BM25가 top-10에 gold를 하나도 못 넣는 62개 인스턴스에서 임베딩은 66%를 찾는다.** 두 의미 신호는 상보적이다. LTR이 실제로 이 여지를 활용한다 (§5).

> `corr(embed, bm25)`의 pooled 피어슨은 +0.110으로 훨씬 낮게 나온다. BM25 점수는 인스턴스마다 척도가 제각각이라(gold 평균 201) 한 덩어리로 섞으면 상관이 파괴된다. 인스턴스 안에서 재는 것이 맞다.

---

## 5. 목적 조건부 전파 vs 전역 중요도 — 사실이지만 메커니즘은 다르다

| 비교 | 차이 (MRR) | 95% CI | p |
|---|---|---|---|
| **ppr − pagerank** | **+0.281** | [+0.229, +0.337] | < 0.001 |
| ppr − combined (제품 설정) | +0.299 | [+0.249, +0.354] | < 0.001 |
| **ppr − B2 (PR+BC)** | **+0.270** | [+0.217, +0.326] | < 0.001 |
| ppr − bc | +0.239 | [+0.185, +0.298] | < 0.001 |
| ppr − complexity | +0.134 | [+0.068, +0.202] | < 0.001 |
| ppr − bm25 | −0.063 | [−0.123, +0.001] | 0.055 |

LTR 내부에서 graph 그룹만 떼어도 같다:

```
LTR graph global only (ppr 제외)   MRR 0.272
LTR graph only        (ppr 포함)   MRR 0.532      +0.260
```

**"코드베이스 전체에서 중요한 파일"과 "지금 목적에 중요한 파일"은 다르다** — 수치는 초판보다 강해졌다 (+0.259 → +0.281).

### ⚠ 그런데 PPR이 하는 일은 "1위 한 칸 바꾸기"다

초판은 `corr(ppr, pagerank)` = xarray 0.251 / pytest 0.615를 근거로 *"중복이 아니다"*라고 썼다. **그 수치는 pooled 피어슨이고, 가장 약한 통계였다.**

| 계산 방식 | xarray | pytest |
|---|---|---|
| pooled 피어슨 (초판 기재값) | 0.251 | 0.615 |
| pooled 스피어만 | 0.816 | 0.697 |
| **인스턴스별 스피어만 중앙값** | **0.945** | **0.933** |

PPR 값 분포가 극단적으로 치우쳐(선택된 config가 `topk1` — teleport 질량 전부를 파일 하나에) 피어슨만 낮게 나온 것이다. **순위 기준으로 PPR은 PageRank와 0.94로 거의 같다.**

메커니즘을 직접 측정하면 명확하다:

```
BM25와 embed의 top-1 파일이 다르다        82.5%  (일치 17.5%)
그런데 seed된 파일이 PPR에서 1위가 된다    88.2%  (top-5는 100%)
그런데 두 PPR(embed seed vs BM25 seed)
        의 순위 상관                       0.979
```

seed를 완전히 다른 것으로 갈아도 PPR 전체 순위는 0.98로 같다. **PPR = PageRank 순서 + seed 파일 하나를 맨 앞으로.** MRR이 크게 달라지는 것은 MRR이 1위에만 좌우되기 때문이다.

**따라서 §5의 +0.281은 사실이지만, "그래프 전파가 목적에 맞게 재편된다"는 뜻이 아니다. "맨 앞 한 자리가 바뀐다"는 뜻이다.** 이 구분을 결론에 반드시 포함할 것.

### seed 샤프닝

`sharpen_seed`(top-k 절단 + 거듭제곱)는 train fold 0~4로만 선택했다 (`ppr_select.py:48-52`가 test 혼입 시 예외를 던져 코드로 강제).

동일 그리드(14 config)·동일 fold에서:

| config | BM25 seed | **embed seed** |
|---|---|---|
| **topk1_pow1** | 0.3882 ± 0.0869 | **0.3961 ± 0.0718** ← 양쪽 다 선택 |
| topk3_pow1 | 0.2315 | **0.2611** |
| topk5_pow1 | 0.2091 | **0.2268** |
| topk20_pow1 | 0.1610 | 0.1599 |
| topk0_pow1 (샤프닝 없음) | 0.1413 | 0.1231 |

embed가 근소 우세이고 fold 간 편차가 더 작다. 5/5 fold에서 `topk1_pow1`이 1위.

**패턴**: embed는 뾰족한 config(topk 1·3·5)에서 이기고 퍼진 config(topk 0·20·50)에서 진다. 정확도가 상위 몇 개에 몰려 있고 꼬리는 BM25보다 덜 정확하다.

> 선택된 `topk1_pow1`은 fold 간 표준편차가 0.07~0.09로 다른 config(0.015~0.036)의 2~5배다. "top-1 파일에만 teleport 질량 전부"라는 극단 설정이라 불안정한 것이 당연하다.

---

## 6. Hybrid LTR과 Ablation — **가설 기각, 최종적으로 음의 기여**

### 최종 성능

`LTR full` — **MRR 0.753, R@10 0.885, NDCG@10 0.753**

| 비교 | 차이 (MRR) | 95% CI | p |
|---|---|---|---|
| LTR − B2 (deterministic baseline) | **+0.626** | [+0.576, +0.677] | < 0.001 |
| LTR − complexity | +0.490 | [+0.430, +0.550] | < 0.001 |
| LTR − ppr | +0.356 | [+0.304, +0.411] | < 0.001 |
| LTR − bm25 | **+0.294** | [+0.240, +0.350] | < 0.001 |

세 단계의 누적: **MRR 0.594 → 0.730 (임베딩) → 0.753 (리랭커)**, R@10 0.774 → 0.885.

### Ablation (229, `no_paths`, MRR)

| 구성 | MRR | full 대비 | 95% CI | p |
|---|---|---|---|---|
| LTR full | 0.753 | — | — | — |
| **LTR −graph** | **0.778** | **+0.024** | [+0.0002, +0.049] | **0.046** |
| **LTR −ppr** | **0.773** | **+0.020** | [+0.003, +0.036] | **0.023** |
| LTR −complexity | 0.745 | −0.008 | [−0.036, +0.018] | 0.553 |
| **LTR −semantic** | **0.560** | **−0.194** | [−0.246, −0.144] | **< 0.001** |
| semantic only | 0.672 | −0.082 | [−0.119, −0.045] | < 0.001 |
| graph only | 0.532 | −0.222 | [−0.272, −0.174] | < 0.001 |
| complexity only | 0.288 | −0.466 | [−0.521, −0.410] | < 0.001 |
| graph global only (ppr 제외) | 0.272 | −0.482 | [−0.540, −0.426] | < 0.001 |

### 기각된 가설 — 반증이 단계마다 강해졌다

임베딩을 도입한 목적은 *"PPR seed가 BM25에서 나와 구조 신호가 의미 신호와 독립이
아니므로 graph 그룹의 한계 기여가 유의하지 않다(−0.020, p=0.074)"*의 해소였다.
seed를 독립적인 신호로 갈면 유의해질 것으로 예상했다.

**세 단계 모두 예상과 반대로 갔고, 방향이 일관됐다.**

```
BM25만      -0.020  p=0.074     빼면 손해 (유의하지 않음)
+ 임베딩    +0.006  p=0.743     효과 없음
+ 리랭커    +0.024  p=0.046     빼는 게 이득 (유의함)
```

의미 신호가 강해질수록 구조 피처는 **무용을 지나 유해로** 넘어갔다.
`-ppr`도 같다 (+0.020, p=0.023) — 이 프로젝트가 핵심으로 내세운 목적 조건부 전파
하나만 빼도 성능이 유의하게 오른다.

피처 기여도가 이유를 보여준다:

```
rerank       0.309 ┐
rerank_rank  0.250 ┘ 55.8%
embed        0.089 ┐
embed_rank   0.088 ┘ 17.7%      semantic 합계 약 76%
ppr          0.042
pagerank     0.028
나머지 12개  합계 약 19%
```

**정직하게: 이 실험은 "구조가 독립적으로 기여한다"를 세 번 시도해 세 번 실패했고,
마지막에는 반대 결론이 유의수준을 넘었다.**

> ⚠ 단 `-graph`의 CI 하한이 **+0.0002**로 0에 아슬아슬하게 걸쳐 있다(p=0.046).
> "빼면 유의하게 좋아진다"를 강한 증거로 쓰기에는 경계선이다. 확실한 것은
> **"구조가 도움이 된다는 증거가 없다"**이고, "해롭다"는 약한 증거다.

## 7. 어휘 비중첩 구간 — **확정**

`non_overlap` = 이슈 텍스트에 그 파일의 식별자가 **한 번도 등장하지 않는** gold 파일 구간.
BM25가 원리상 찾을 수 없는 곳이다.

> **초판의 오진.** 이 구간을 17 인스턴스로 보고 "repo가 2개뿐이라 작다"고 적었는데,
> 실제 원인은 **어휘 중첩 라벨을 xarray에만 계산해둔 것**이었다 (`vocab_overlap.csv`가
> 110개 인스턴스만 덮었고 pytest 119개는 구간 분석에서 통째로 빠져 있었다).
> 이미 가진 데이터로 라벨을 마저 채우자 **40 인스턴스 / gold 85**가 됐다 — 사전에 정한
> 노이즈 기준(20)을 넘었고, 이 구간의 모든 검정에서 `underpowered` 표시가 사라졌다.
> repo 추가도 API 비용도 필요 없었다.

### 단일 피처: BM25가 가장 크게 무너지고 전역 구조가 상대적으로 올라온다

| 피처 | 전체 | non_overlap | 변화 |
|---|---|---|---|
| **rerank (크로스 인코더)** | 0.651 | **0.375** | ×0.58 — **구간 내 1위** |
| embed (Qwen3) | 0.523 | 0.263 | ×0.50 |
| ppr | 0.397 | 0.215 | ×0.54 |
| **bc** | 0.158 | **0.196** | **×1.24 상승** |
| **combined** | 0.098 | **0.191** | **×1.95 상승** |
| complexity | 0.263 | 0.173 | ×0.66 |
| **in_deg** | 0.100 | **0.156** | **×1.56 상승** |
| **bm25** | 0.459 | **0.153** | **×0.33 — 붕괴 폭 최대** |
| pagerank | 0.116 | 0.121 | ×1.04 |

**BM25는 3배로 무너지고, 임베딩은 2배 하락에 그치며 구간 내 1위가 된다.**
전역 구조 지표(`bc`·`combined`·`in_deg`)는 절대값이 낮지만 **상대적으로 올라온다.**

### 조합 모델: 구조가 의미와 대등해지지만, 여전히 한계 기여는 없다

| 구성 | non_overlap MRR |
|---|---|
| LTR full | 0.308 |
| LTR −graph | 0.316 |
| LTR −semantic | 0.204 |
| **graph only** | **0.223** |
| **semantic only** | **0.222** |
| graph global only (ppr 제외) | 0.104 |

**`graph only`(0.223)와 `semantic only`(0.222)가 사실상 같다.** 전체 구간에서는
semantic이 압도했는데(0.551 vs 0.532), 어휘가 겹치지 않으면 둘이 대등해진다.

그러나 ablation은 §6과 같은 답을 낸다:

| 비교 | ΔMRR | 95% CI | p |
|---|---|---|---|
| −graph − full | +0.009 | [−0.046, +0.064] | 0.739 |
| −complexity − full | +0.014 | [−0.043, +0.077] | 0.649 |
| **−semantic − full** | **−0.103** | [−0.180, −0.026] | **0.006** |

### 결론

**확정된 것**
1. BM25는 어휘 비중첩 구간에서 3배로 붕괴한다 (0.459 → 0.153)
2. 임베딩이 그 자리를 부분적으로 메우고(0.263), **크로스 인코더가 더 메운다(0.375)** — §8
3. 구조 신호는 이 구간에서 의미 신호와 **대등한 수준까지 올라온다** (graph only 0.223
   vs semantic only 0.222 — 단 이는 리랭커 도입 **이전** 수치다)

**확정된 반증**
4. 그럼에도 **구조는 조합 모델에서 한계 기여가 없다** (−graph +0.009, p=0.704). §6과 같다.
5. 리랭커를 넣으면 이 구간에서 **semantic만으로 full과 완전히 동률**이 된다:

   ```
   LTR semantic only   0.3340
   LTR full            0.3318      차이 +0.0022,  p=0.980
   ```

3과 4·5는 모순이 아니다. 구조는 단독으로는 쓸 만하지만 의미 신호가 이미 잡아낸 것과
겹쳐서, 함께 넣었을 때 추가로 설명하는 몫이 없다. 그리고 의미 신호가 강해질수록
그 몫은 0에 가까워진다.

---

## 8. 크로스 인코더 — 어휘가 겹치지 않을 때 유일하게 통하는 것

바이 인코더(임베딩)는 쿼리와 문서를 **따로** 벡터로 만들어 코사인을 잰다. 그래서 문서를
미리 인코딩해 캐시할 수 있지만(6,308 blob을 한 번만) 둘이 서로를 보지 못한다.
크로스 인코더는 (쿼리, 문서)를 **함께** 넣어 관련성을 직접 낸다 — 대신 캐시가 원리상
불가능해 상위 50개만 재채점한다.

| 피처 | 전체 | overlap | **non_overlap** |
|---|---|---|---|
| **rerank** | **0.651** | **0.642** | **0.375** |
| embed | 0.523 | 0.526 | 0.263 |
| bm25 | 0.459 | 0.473 | 0.153 |
| ppr | 0.397 | 0.395 | 0.215 |

**어휘 비중첩 구간에서 임베딩 대비 +0.111이다.** 이슈와 코드를 함께 읽는 방식이
어휘도 임베딩도 힘을 못 쓰는 자리에서 실제로 작동한다 — 이 단계를 붙인 이유였다.

### 설계에서 결정적이었던 두 가지 (둘 다 실측으로 뒤집혔다)

**① 문서에 파일 경로를 붙여야 한다.** 처음에는 코드 청크만 보냈고, 리랭커가 임베딩보다
크게 **못했다**(3 인스턴스 MRR 0.14 vs 0.36). 어느 파일에서 온 조각인지 모른 채
코드만 보고 판단하고 있었다. 경로를 접두하자 시험한 4개 모델이 **전부** 올랐다
(+0.045 ~ +0.264). `no_paths`는 **쿼리**에서 경로를 지운 조건이므로 문서 쪽 경로는 누출이 아니다.

**② 모델 선택이 결정적이다.** 40 인스턴스 검증:

```
embed (동일 후보 50개 내)     MRR 0.605
cohere/rerank-4-fast         MRR 0.727     <- 채택
cohere/rerank-v3.5           MRR 0.563     <- 임베딩보다 못함
```

기본값으로 골랐던 `voyageai/rerank-2.5`는 3 인스턴스 예비 비교에서 최하위권이었다.
"코드에 강하다고 알려진 모델"을 고르는 대신 재보고 골랐다.

### ⚠ 후보 집합이 임베딩에 종속된다

상위 50개도, 각 파일을 대표하는 청크도 임베딩이 고른다. `rerank`는 `embed`와 독립이
아니며, 이는 **`ppr`이 `bm25` seed에 종속됐던 것과 같은 구조**다(§5·§9-2).
cascade의 본질적 성질이라 없앨 수 없고, semantic 그룹 **내부**의 한계 기여를 해석할 때
반드시 감안해야 한다. 상한도 여기서 정해진다 — top-50의 gold 포함률은 전체 0.953,
non_overlap 0.869이다.

---

## 9. 제품에 대한 함의

**`file_rank`(제품이 실제 출력하는 읽기 순서)가 모든 지표에서 최하위다** — MRR 0.054로 원시 `pagerank`(0.116)의 절반도 안 된다.

원인은 위상정렬 방향이다. condensation 간선을 뒤집어 leaf-first가 되는데(`ReadingSequencer.cpp`), 이는 "패치되는 파일"과 구조적으로 반대다. `docs/reading-direction.md` 참조.

⚠ **단 SWE-bench는 *관련성*을 재는 벤치마크이지 *읽는 순서*를 재지 않는다.** 온보딩 목적이라면 leaf-first가 옳을 수 있다. 벤치마크 점수를 근거로 뒤집을 사안이 아니며 **제품 결정으로 남아 있다.**

§6을 근거로 한 실무적 함의는 별개이고, 리랭커 도입 후 더 분명해졌다:

**파일 추천 품질만 놓고 보면 그래프 그룹을 빼는 편이 낫다** (−graph +0.024, p=0.046).
`rerank` 하나가 모델 gain의 31%, semantic 그룹이 76%를 가져간다. 그래프 계산을 유지할
근거는 **랭킹 성능에 없다** — 구조 시각화, 읽기 순서, 의존성 설명 등 다른 데서 찾아야 한다.

단 §6의 단서를 함께 읽을 것: CI 하한이 +0.0002라 "해롭다"는 약한 증거다.
확실한 것은 "도움이 된다는 증거가 없다"이다.

---

## 10. 한계

1. **구조 신호의 독립 기여를 보이지 못했다** (§6). 이 실험의 원래 목표였고, 세 단계
   모두 기각됐다. 어휘 비중첩 구간에서도 같다 (§7). 최종적으로는 음의 기여(p=0.046)까지
   갔으나 CI 하한이 0에 걸쳐 있어 "해롭다"로 단정하지는 않는다.
2. **PPR의 실질 동작이 "1위 한 칸 교체"다** (§5). 초판의 "중복이 아니다"는 서술은 pooled 피어슨에만 의존했다.
3. **구간 정의에 민감하다.** `vocab_definition_sweep.csv` — 정의 파라미터에 따라 크기가 gold **2개(0.8%)~191개(73.5%)**까지 움직인다.
4. **라벨 편향.** "수정된 파일" ≠ "이해하기 위해 읽어야 할 파일". gold는 정답의 부분집합이고 맥락 파일을 과소평가한다 (Kochhar et al., ASE 2014).
5. **`complexity`는 부분적으로 크기 대리변수다.** gold complexity 평균 0.775 vs 비gold 0.453이라 순수 크기 효과는 아니나, 크기 통제 비교를 별도로 봐야 한다 (`ltr_ppr_nopaths_size_control.csv`).
6. **repo 2개뿐이다** (xarray, pytest). 일반화 주장 불가. 단 §7의 구간 크기 문제는
   repo 부족이 아니라 라벨 누락이었고, 이미 해소됐다.
7. **`rerank`는 상위 50개만 채점한다** (§8). 후보 밖 파일은 NaN이고 랭킹에서 최하위로
   밀린다. 즉 §3의 `rerank` 수치는 후보 밖을 포기한 값이며, 상한이 전체 0.953 /
   non_overlap 0.869로 고정돼 있다.
8. **`rerank`의 후보와 대표 청크를 `embed`가 고른다** (§8). semantic 그룹 내부의
   한계 기여 해석에 이 종속성을 감안해야 한다.
9. **임베딩·리랭킹 모두 외부 API다** (§2). 바이너리처럼 핀할 수 없다. 벡터 캐시 + 인코더 지문으로 완화.

---

## 11. 재현 방법

```bash
bench/.venv/bin/python -m pytest bench/ -q      # 시스템 python3는 PEP 668로 잠김

# 1. 임베딩 seed  (OPENROUTER_API_KEY 필요 — .env.example 참조)
bench/.venv/bin/python -m bench.seed.run_embed \
  --add-repo pydata/xarray=bench/repos/xarray \
  --add-repo pytest-dev/pytest=bench/repos/pytest \
  --features-csv bench/data/features_all.csv \
  --backend openrouter --model qwen/qwen3-embedding-8b \
  --dimensions 1024 --concurrency 8 --out-dir bench/data/seed_all
#   --dry-run 을 붙이면 인코딩 없이 청크·토큰·비용만 센다

# 2. seed 샤프닝 선택 (train fold만)
bench/.venv/bin/python -m bench.features.ppr_sweep  ... --seed-weight-col embed
bench/.venv/bin/python -m bench.features.ppr_select --sweep bench/data/ppr_sweep_embed14.csv

# 3. 선택된 config로 229개 전체 재스캔 + PPR
bench/.venv/bin/python -m bench.collect.pipeline \
  --repo <repo> --repo-dir <clone> --split test \
  --ppr-bin bench/bin/TelescodePPR \
  --seed-csv bench/data/seed_all/embed_seed__no_paths.csv \
  --seed-weight-col embed --seed-topk 1 --seed-power 1.0

# 3b. 리랭킹 seed (상위 50개 재채점)
bench/.venv/bin/python -m bench.seed.run_rerank \
  --add-repo pydata/xarray=bench/repos/xarray \
  --add-repo pytest-dev/pytest=bench/repos/pytest \
  --embed-seed bench/data/seed_all/embed_seed__no_paths.csv \
  --cache bench/data/seed_all/embed_blobs.npz \
  --model cohere/rerank-4-fast --top-n 50 --concurrency 4 \
  --out-dir bench/data/seed_all
#   --dry-run 으로 비용 확인 · --no-path 로 경로 접두 해제(성능 크게 하락)

# 4. LTR + ablation
bench/.venv/bin/python -m bench.ltr.run_ltr \
  --features bench/data/features_all_embedppr.csv \
  --manifest bench/data/instances_all_embedppr.csv \
  --bm25-seed  bench/data/seed_all/bm25_seed__no_paths.csv \
  --embed-seed  bench/data/seed_all/embed_seed__no_paths.csv \
  --rerank-seed bench/data/seed_all/rerank_seed__no_paths.csv \
  --overlap bench/data/vocab_overlap.csv --overlap-condition no_paths \
  --splits bench/data/splits_all.csv --out-prefix bench/data/ltr_embed
```

바이너리는 `bench/bin/`의 pin된 것만 사용 (`PINNED_FROM.txt`에 SHA·md5).

| 파일 | 내용 |
|---|---|
| `seed_all/embed_seed__*.csv` | §3·§4 임베딩 seed |
| `seed_all/embed_blobs.npz` | 벡터 캐시 (지문 포함, 197MB) |
| `embed_per_instance.csv` | §4 인스턴스별 MRR/R@10 |
| `ppr_sweep_embed14.csv` | §5 seed 샤프닝 (14 config) |
| `features_all_embedppr.csv` | §6 최종 피처 매트릭스 |
| `seed_all/rerank_seed__no_paths.csv` | §8 리랭킹 seed |
| `ltr_rerank*.csv` / `ltr_rerank*.json` | §6·§8 최종 ablation·유의성 |
| `ltr_embed*.csv` | 리랭커 이전 단계 산출물 |
| `vocab_segment_eval.csv` | §7 |
| `ltr_ppr_nopaths_*.csv` | 초판(BM25 only) 산출물 |

---

## 12. 미해결

- **구조 신호를 살릴 수 있는가** (§6) — 세 번의 시도에서 모두 실패했고, 마지막에는
  빼는 편이 유의하게 나았다. 현재 형태의 PPR은 랭킹에 독립 기여를 못 한다.
  seed와 무관한 구조 피처(호출 거리, 모듈 응집도 등)를 따로 시험해볼 수 있다.
- **PPR을 "1위 교체" 이상으로 만들 수 있는가** (§5) — `topk1`이 train CV에서 이기는 한
  전파는 부수적이다. d(damping)를 함께 튜닝하거나 seed를 더 넓게 유지하는 설계가 필요하다.
- **읽기 방향 제품 결정** (§8) — `docs/reading-direction.md`
- 온보딩용 순서와 작업 지원용 순서를 한 모델로 낼지, 모드를 분리할지
- UI 통합 — `AlgoRunner::run(dbPath, cfg)`에 목적 질의 인자가 없다. 시그니처 변경 필요
- 임베딩·리랭킹 비용/지연을 제품에서 감당할 것인가 — 현재 코퍼스 기준 임베딩 $0.97
  (6,308파일, 약 25분, 캐시 가능) + 리랭킹 $1.07 (229 질의, 152초, **캐시 불가**).
  리랭킹은 질의마다 다시 계산해야 하므로 제품에서는 질의당 비용이 된다.
- 리랭킹 후보 수 N — 현재 50. 상한이 non_overlap 0.869이라 N을 키우면 여지가 있으나
  비용이 선형으로 는다.
