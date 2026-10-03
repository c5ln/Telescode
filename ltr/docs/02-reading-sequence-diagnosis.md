# Reading Sequence — 현행 알고리즘 진단 & AI 고도화 방향

> 측정 대상: `pystdlib.db` (CPython stdlib, 680 files)
> `output.db`는 스키마가 구버전(`complexity_score` 이전)이라 측정에서 제외

---

## 1. 측정된 실패 모드

### F1. 리프 우선 → 상수 테이블부터 읽으라고 함

#### 원인

`ReadingSequencer.cpp:45`에서 condensation 간선을 뒤집는다.

```cpp
// Reversed: v's SCC → u's SCC so importee SCC is processed first
cond_adj[scc_of[v]].push_back(scc_of[u]);
```

in-degree 0 = "아무것도 import 하지 않는 파일" → **leaf-first / bottom-up**.
PageRank도 `adj` 방향으로 흐르므로(`Scoring.cpp:46`) 피의존 유틸이 점수를 먹는다.
**중요도와 위상순서가 둘 다 리프를 가리킨다.**

#### 실측

| rank | file | 비고 |
|------|------|------|
| 1 | `reprlib.py` | |
| 2 | `encodings/aliases.py` | 551줄 순수 dict |
| 3 | `stat.py` | 상수 |
| 10 | `token.py` | `cx=0.00` |
| **40** | `locale.py` | comb=0.885 (최고점) |
| **44** | `os.py` | comb=0.741 |

---

### F2. SCC 붕괴 → 위상정렬이 정보를 주지 못함

#### 실측

score 단조감소 연속 구간으로 SCC 확장 블록을 역산:

| 블록 크기 | rank 구간 | 비중 |
|-----------|-----------|------|
| 385 | 296~680 | 57% |
| 164 | 40~203 | 24% |
| 39 | 1~39 | 6% |

전체의 **81%가 단 두 덩어리**. `Graph.cpp:65`의 `scc.size() > 10` 경고가 이미 신호를 주고 있었음.

블록 내부에는 위상 제약이 없고 순전히 `combined_score` 정렬이므로, 최고점 파일들이 rank 40~49에 **연속으로** 박힌다.

```
comb=0.885  rank= 40   locale.py
comb=0.841  rank= 41   abc.py
comb=0.827  rank= 42   codecs.py
comb=0.741  rank= 44   os.py
comb=0.696  rank= 45   functools.py
```

#### 원인

`Graph.cpp:102` `add_edge`가 모든 간선을 weight 1로 삽입하고, SQL에 `DISTINCT`가 걸려 있어 **50번 호출이나 1번 호출이나 동일**. 약한 간선 하나가 거대 사이클을 닫는다.

---

### F3. 파일 절반 이상에서 점수 신호 소멸

#### 실측

```
combined_score 고유값: 271 / 680
score = 0.000000 인 파일: 382개 (56.2%)
pagerank(file) min=0.000282 max=0.052833  → 187.6x
```

`combined_score == 0`인 382개 구간은 `loc` 내림차순 → 파일명 알파벳순(`ReadingSequencer.cpp:79-84`)으로 결정된다. **사실상 순서가 없음.**

#### 원인

`ScoreCombiner::combine`의 min-max 정규화(`Scoring.cpp:256`)가 187배 스팬의 PageRank를 소수 이상치 기준으로 압축 → 하단이 전부 바닥에 붙는다.
`ComplexityScorer`는 이미 `percentile_rank_normalize`를 쓰는데(`Scoring.cpp:84`) `ScoreCombiner`만 min-max라 **일관성도 없다.**

---

### F4. `complexity_score`가 sequencer에 미연결

`ComplexityScorer::computeAndWrite`(`Scoring.cpp:175`)는 `file` 테이블에 쓰기만 하고, `AlgoRunner::run`은 다시 읽지 않는다.

#### 실측

```
앞쪽 절반 평균 complexity = 0.598
뒤쪽 절반 평균 complexity = 0.266
```

→ **어려운 파일부터 읽으라고 하고 있음.**

---

> pystdlib은 순환 의존이 극심한 극단 케이스지만, **F1·F3·F4는 그래프 모양과 무관하게 재현되는 구조적 문제**다.

---

## 2. 재구성 — 관련성과 순서의 분리

### 문제 인식

`combined_score` 하나가 서로 다른 두 질문에 동시에 답하려 한다.

| 질문 | 결정하는 것 | 성격 |
|------|-------------|------|
| 무엇을 읽어야 하는가 | 부분그래프 선택 | 의미적 — **목적에 의존** |
| 어떤 순서로 읽어야 하는가 | 선행관계 정렬 | 구조적 — 목적과 무관 |

관련성 신호를 위상 제약에 밀어넣으면 잡아먹힌다(= F2 증상).

### 3단 구조

```
[1단] seed 도출        목적 텍스트 → 파일별 seed 가중치      학습 필요
        임베딩 유사도 + 식별자/docstring BM25 + LLM 재순위
        재현율 위주로 넓게
           ↓
[2단] 구조 전파        Personalized PageRank from seed      결정론적
        ★ 이 프로젝트의 존재 이유
        RAG: 목적과 "비슷한" 파일
        PPR: 그걸 이해하는 데 "구조적으로 필요한" 파일
           ↓
[3단] 순서 결정        부분그래프에만 SCC + 위상정렬        결정론적
        대상이 680 → 30~50개로 줄어 F2도 완화
```

**2단의 근거:** 로그인 버그를 고치려면 `auth/login.py`가 상속하는 세션 기반 클래스를 읽어야 하는데, 그 파일에 "login"이 한 번도 안 나올 수 있다. 임베딩은 못 찾고 그래프는 찾는다.

### PageRank 개인화 수식

```
현재: PR(v) = (1-d)/N    + d·Σ PR(u)/out(u) + d·dangling/N
개선: PR(v) = (1-d)·p[v] + d·Σ PR(u)/out(u) + d·dangling·p[v]
```

`Scoring.cpp:26`의 `pr(N, 1.0/N)`, `:39`의 `base` 두 줄 수정.
teleport가 균등하지 않으면 **바닥값이 파일마다 달라져 F3의 382개 동점이 깨진다.**

---

## 3. 가중치 학습 — git이 라벨을 준다

### 왜 가중치가 안 정해졌는가

감이 없어서가 아니라 **목적함수가 없었기 때문.**
"좋은 읽기 순서"는 그 자체로 정답이 정의되지 않아 α=0.6이 0.7보다 나은지 판정 불가.

**목적을 입력으로 받는 순간 채점이 가능해진다.** 즉 목적 조건화는 기능 추가이면서 동시에 가중치 문제의 해법이다.

### 지도 신호

```
커밋 메시지 / PR 제목      →  목적 질의 (intent)
그 커밋이 변경한 파일 집합  →  관련성 정답 (relevance label)
```

`repos/sherlock/.git` 확인 완료 — 스캔 대상 repo에 히스토리가 남아 있어 즉시 적용 가능.

이제 α, β, damping, 간선 타입 가중치를 **NDCG@k / Recall@k로 채점해서 고를 수 있다.**

### 딸려오는 공짜 피처: co-change

같은 커밋에서 함께 바뀐 파일 쌍의 빈도 = **행동적 의존성.**
덕타이핑·동적 디스패치 때문에 정적 파서가 원리상 못 잡는 관계를 LLM 없이 잡아낸다.
(`docs/CALLS_RESOLUTION.md`의 Python recall 문제와 직결)

### ⚠ 라벨 편향 (반드시 처리)

**"변경된 파일" ≠ "이해하기 위해 읽어야 할 파일"**
보통 1개를 고치려고 10개를 읽으므로 이 라벨은 **정답의 부분집합**이고 맥락 파일을 과소평가한다.

→ 변경되지 않은 파일을 negative로 학습시키면 **안 된다.**
→ positive-unlabeled 문제로 다루거나, positive만 쓰고 랭킹 손실로 학습.
→ 편향이 들어가면 "구조적 맥락은 필요 없다"를 모델이 학습해 프로젝트 주장과 정반대로 간다.

---

## 4. 학습 / 결정론 경계

| 구분 | 대상 | 근거 |
|------|------|------|
| **학습** | seed 도출 (1단) | 본질적으로 의미 문제 |
| **학습** | 피처 결합 가중치 | 단, **파라미터 10~30개 선형 결합으로 충분**. 신경망·LLM 불필요 |
| **결정론** | PPR 전파 (2단) | 입력이 주어지면 증명 가능하게 옳음 |
| **결정론** | 위상정렬 (3단) | 위와 동일 |

해석 가능성을 잃으면 "왜 이 순서인지"를 설명할 수 없고, 이해를 돕는 도구로서 치명적이다.

---

## 5. 반드시 먼저 확인할 것 — 베이스라인

목적 질의에 대해 **순수 임베딩 검색**과 붙여볼 것.

- PPR + 구조가 순수 검색을 **못 이기면** → "코드베이스는 그래프로 이해해야 한다"는 핵심 주장이 미검증 상태
- **이기면** → "구조 전파가 검색 단독 대비 NDCG +X" 가 Telescode가 RAG 검색 도구가 아니라는 유일한 근거

예상: 전체 평균이 아니라 **질의어와 어휘가 겹치지 않는 선행 파일 구간**에서 이긴다.
그 구간을 따로 떼어 측정할 것.

---

## 6. AI 없이 즉시 고칠 항목

**AI 붙이기 전에 선행. 안 고치면 AI 효과를 측정할 수 없다.**

| # | 항목 | 위치 | 해결 |
|---|------|------|------|
| 1 | min-max 정규화 | `Scoring.cpp:256` | → percentile rank. `ComplexityScorer`와 일관성 확보, F3 크게 완화 |
| 2 | `complexity_score` 미연결 | `AlgoRunner.cpp:134` | sequencer에 연결. 동등 중요도면 쉬운 것 먼저 (F4) |
| 3 | `loc` 내림차순 tie-break | `ReadingSequencer.cpp:81` | 동점 시 큰 파일부터 읽으라는 의미 — 재검토 |
| 4 | 간선 가중치 전무 | `Graph.cpp:102` | `INHERITS > CALLS > IMPORTS`, 호출 횟수(`DISTINCT` 제거), `if TYPE_CHECKING` 제외 (F2) |
| 5 | 읽기 방향 | `ReadingSequencer.cpp:45` | bottom-up이 설계 의도인지 구현 결과인지 **의식적으로 선택** |

---

## 7. 이해 비용을 목적함수로

병목이 "이해"라면 관련성만으로는 부족하다. 같은 파일 집합도 순서에 따라 총 이해 비용이 다르다.

> **목적의 선행 폐포를 덮으면서 Σ(이해 비용)을 최소화하는 순서**

이렇게 놓으면 `complexity_score`가 히트맵 색깔이 아니라 목적함수의 **비용 항**으로 제자리를 찾는다 (F4의 이론적 해결).

---

## 8. 참고 논문

### 목적 → 파일 랭킹 (bug localization / feature location)

| 논문 | 왜 |
|------|-----|
| ★ **Ye, Bunescu, Liu** — Learning to Rank Relevant Files for Bug Reports using Domain Knowledge (FSE 2014) | 텍스트+변경이력+co-change 피처를 LTR로 학습. **가중치 문제의 완성된 답안** |
| **Zhou, Zhang, Lo** — BugLocator (ICSE 2012) | 이 분야 기준점. 평가 셋업(Top-k/MAP/MRR)의 원형. 넘어야 할 베이스라인 |
| **Saha et al.** — BLUiR (ASE 2013) | 구조 필드 색인. 그래프 전에 이걸 이겨야 그래프가 정당화됨 |
| ⚠ **Kochhar, Le, Lo** — Potential Biases in Bug Localization (ASE 2014) | 라벨 편향 실증. **평가 설계 전 필독** — 안 읽으면 실험 재수행 |
| **Lee et al.** — Bench4BL (ISSTA 2018) | 재현성 연구 + 벤치마크. 베이스라인 수치 출처 |

### seed → 구조 전파

| 논문 | 왜 |
|------|-----|
| ★ **Robillard** — Topology Analysis of Software Dependencies (TOSEM 2008) | 구상 중인 PPR-from-seed와 거의 동일. **텍스트 없이 구조만으로** 추천 |
| **Haveliwala** — Topic-Sensitive PageRank (WWW 2002) | 개인화 teleport `p[v]`의 원 논문 |
| **Jeh & Widom** — Scaling Personalized Web Search (WWW 2003) | 위와 동일 + 계산 절약 |
| **Inoue et al.** — Component Rank (ICSE 2003) | PageRank의 소프트웨어 적용 계보 |
| **Tong, Faloutsos, Pan** — Fast Random Walk with Restart (ICDM 2006) | 실시간 질의용 근사 계산 |

### co-change

| 논문 | 왜 |
|------|-----|
| ★ **Zimmermann, Zeller et al.** — Mining Version Histories to Guide Software Changes (ICSE 2004 / TSE 2005) | co-change의 정본. Python 덕타이핑 구멍을 LLM 없이 메우는 근거 |
| **Kagdi, Gethers, Poshyvanyk** 계열 | conceptual + evolutionary coupling 결합 |

### 개발자의 실제 코드 읽기

| 논문 | 왜 |
|------|-----|
| ★ **Kersten & Murphy** — Using Task Context to Improve Programmer Productivity (FSE 2006, Mylyn/DOI) | **가장 가까운 선행 사례.** 차별점 정리 필수 |
| **Sillito, Murphy, De Volder** — Asking and Answering Questions during a Programming Change Task (TSE 2008) | 개발자 질문 44종 분류 → "목적" 입력 타입 설계에 직접 사용 |
| **Robillard, Coelho, Murphy** — How Effective Developers Investigate Source Code (TSE 2004) | 체계적 탐색 > 기회주의적 탐색. **"순서 추천이 실제로 도움된다"의 실증 근거** |
| **Ko, Myers, Coblenz, Aung** (TSE 2006) | 유지보수 중 정보 탐색 행태 |

### 이해 비용

| 논문 | 왜 |
|------|-----|
| **Campbell** — Cognitive Complexity (SonarSource 2018) | 순환복잡도가 이해 난이도 지표로 부적절한 이유 + 대안 |

### LLM 시대 평가 데이터

| 논문 | 왜 |
|------|-----|
| ★ **Jimenez et al.** — SWE-bench (ICLR 2024) | (이슈 → 패치 파일) 쌍이 이미 정제됨. **라벨 직접 제작 불필요.** 이슈 텍스트가 커밋 메시지보다 "목적"에 가까움 |
| **Xia et al.** — Agentless (2024) | 파일 단위 localization 단계 보유. "LLM에 파일 목록 던지기" 대비 베이스라인 |

### 우선 읽기 4편

1. **Ye et al. (FSE 2014)** — 가중치 학습의 답안
2. **Robillard (TOSEM 2008)** — 구조 전파의 유효성
3. **Kochhar et al. (ASE 2014)** — 평가 설계 전 필수
4. **Kersten & Murphy (FSE 2006)** — 차별점 정리

> ⚠ 이 분야 대부분이 **Java(Eclipse/AspectJ 벤치마크) 기준**이다. Python은 동적 디스패치 때문에 정적 그래프 품질이 낮아 논문 수치가 그대로 재현되지 않을 가능성이 높다.
> → 오히려 기여 포인트: **"정적 그래프가 약한 언어에서 co-change와 LLM 추출 soft edge가 갭을 메운다"**는 덜 다뤄진 영역.

> 서지정보는 대화 중 기억 기반으로 정리한 것이라 게재 연도/학회명이 일부 어긋날 수 있음. 인용 전 확인 필요.

---

## 9. 측정 재현 방법

`sqlite3` CLI가 없어 Python으로 측정했다.

```python
import sqlite3
from collections import Counter
c = sqlite3.connect("file:pystdlib.db?mode=ro", uri=True)

# F1 — 추천 순서 상위
c.execute("""SELECT file_rank, entity_id, pagerank_score, combined_score
             FROM reading_sequence WHERE entity_type='file'
             ORDER BY file_rank ASC LIMIT 15""")

# F2 — SCC 블록 역산 (score 단조감소 연속 구간)
rows = c.execute("""SELECT file_rank, entity_id, combined_score
    FROM reading_sequence WHERE entity_type='file' AND file_rank IS NOT NULL
    ORDER BY file_rank""").fetchall()
blocks, cur = [], [rows[0]]
for prev, r in zip(rows, rows[1:]):
    if r[2] <= prev[2] + 1e-12: cur.append(r)
    else: blocks.append(cur); cur = [r]
blocks.append(cur)

# F3 — 동점 그룹
sc = [r[0] for r in c.execute(
    "SELECT combined_score FROM reading_sequence WHERE entity_type='file'")]
Counter(round(s, 9) for s in sc).most_common(3)

# F4 — 순서 vs 복잡도
c.execute("""SELECT rs.file_rank, f.complexity_score
    FROM reading_sequence rs JOIN file f ON f.file_id = rs.file_id
    WHERE rs.entity_type='file' AND rs.file_rank IS NOT NULL
    ORDER BY rs.file_rank""")
```

---

## 10. 미해결

- **학습 파이프라인의 현재 라벨이 무엇인지** — 이에 따라 다음 설계가 달라짐
- 읽기 방향(bottom-up vs top-down) 최종 결정
- 온보딩용 순서와 작업 지원용 순서를 하나의 모델로 낼지, 모드를 분리할지
