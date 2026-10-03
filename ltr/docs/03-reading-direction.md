# 읽기 방향(bottom-up) — 메커니즘 문서화와 제품 결정 요청

> Issue #37 Phase 0, 항목 0-5.
> **코드는 바꾸지 않았다.** 현재 왜 bottom-up이 되는지와 그 결과를 기록하고,
> 방향 전환 여부는 사람 판단으로 남긴다.

---

## 1. bottom-up이 되는 지점 — 정확히 세 곳

### (1) 간선 방향의 정의 — `Graph.cpp` `add_edge`

파일 그래프의 간선 `A → B`는 **"A가 B를 import/call/inherit 한다"**, 즉
**A가 B에 의존한다**는 뜻이다. 의존하는 쪽이 source다.

### (2) condensation 간선 역전 — `ReadingSequencer.cpp:52`

```cpp
// Edge direction in adj: A→B means A imports/calls B (A depends on B).
// We want to READ B before A, so the condensation must flow B→A (reversed),
// making B's SCC have 0 in-degree and be processed first.
cond_adj[scc_of[v]].push_back(scc_of[u]);   // v→u, 즉 원래 간선의 역방향
```

Kahn 위상정렬은 in-degree 0인 super-node부터 꺼낸다. 역전 후 in-degree 0은
**"아무것도 import 하지 않는 파일"**, 즉 의존 그래프의 리프다.

→ **위상 제약이 "선행 지식부터" = leaf-first = bottom-up을 강제한다.**

### (3) PageRank의 흐름 방향 — `Scoring.cpp` `PageRank::compute`

PageRank는 `g.adj` 방향(A→B)으로 점수를 흘린다. A가 B를 import 하면 B가 점수를
받는다. 즉 **피의존도가 높은 파일**이 높은 점수를 얻는다. 그런데 피의존도가
높은 파일은 대개 그래프 바닥의 유틸리티다.

→ **중요도 신호와 위상 제약이 둘 다 같은 방향(리프)을 가리킨다.**
(2)만 뒤집는다고 (3)이 따라오지 않는다는 점이 중요하다.

---

## 2. 이것이 설계 의도인가, 구현 결과인가

`ReadingSequencer.cpp:49-51`의 주석은 (2)가 **의도적**이었음을 보여준다
("We want to READ B before A"). 즉 방향 자체는 의식적으로 선택되었다.

**의도되지 않은 것은 (2)와 (3)의 중첩이다.** PageRank를 `adj` 방향으로 돌리면
"중요한 파일"의 정의가 "많이 의존되는 파일"이 되는데, 이건 위상정렬이 이미
앞으로 보내는 집합과 거의 같다. 두 신호가 독립적으로 순서를 결정하는 게 아니라
서로를 증폭한다.

실측(`02-reading-sequence-diagnosis.md` F1): rank 1~10이 `reprlib.py`,
`encodings/aliases.py`(551줄 순수 dict), `stat.py`, `token.py`(cx=0.00) 같은
상수 테이블이고, 최고 중요도 파일(`locale.py`, `os.py`)은 rank 40~44에 있었다.

---

## 3. 방향 선택지

| | bottom-up (현행) | top-down |
|---|---|---|
| 위상 in-degree 0 | 아무것도 import 안 하는 파일 | 아무도 import 하지 않는 파일 (엔트리포인트) |
| 첫 화면 | 상수/유틸 | `__main__`, CLI, 서비스 진입점 |
| 전제 | 밑에서부터 쌓아 올린다 | 목적에서 출발해 필요한 만큼 내려간다 |
| 맞는 상황 | 라이브러리 전체 온보딩 | 특정 작업/버그 수행 |
| 리스크 | 왜 읽는지 모른 채 읽음 | 선행 지식 없이 상위 코드를 봄 |

`ReadingSequencer.cpp`에서 (2)의 `cond_adj[scc_of[v]].push_back(scc_of[u])`를
`cond_adj[scc_of[u]].push_back(scc_of[v])`로 되돌리면 위상 방향은 뒤집힌다.
다만 §1-(3) 때문에 **PageRank 방향(`adj` vs `radj`)도 함께 결정해야** 실제로
top-down이 된다. 한쪽만 바꾸면 두 신호가 서로 상쇄된다.

---

## 4. Phase 0에서 관측된 부작용 — F1과 F4의 충돌

Phase 0의 종료 조건 F4는 **"앞쪽 절반의 평균 complexity ≤ 뒤쪽 절반"**, 즉
쉬운 파일을 먼저 읽으라는 요구다. 이를 만족시키려면 `gamma`(이해 비용 항)를
키워야 하는데, **쉬운 파일 = 리프 = 이미 위상적으로 앞에 오는 파일**이므로
F1(상수 테이블 우선) 증상이 오히려 심해진다.

pystdlib.db 실측. `corr_*`는 `combined_score`와의 Pearson 상관,
`PRtop15@80`은 PageRank 상위 15개 중 읽기 순서 80위 안에 드는 개수다.

| gamma | corr_pr | corr_bc | corr_cx | `os.py` | `locale.py` | PRtop15@80 | F4 front/back |
|---|---|---|---|---|---|---|---|
| 0 | +0.353 | +0.438 | +0.674 | 43 | 40 | 13/15 | 0.598 / 0.266 ✗ |
| **0.2 (채택)** | **+0.352** | **+0.430** | **+0.581** | **54** | **53** | **14/15** | 0.551 / 0.312 ✗ |
| 0.5 | +0.336 | +0.399 | +0.383 | 85 | 81 | 11/15 | 0.533 / 0.330 ✗ |
| 1.0 | +0.244 | +0.265 | −0.087 | 151 | 147 | 3/15 | 0.435 / 0.428 ✗ |
| 1.5 | +0.109 | +0.086 | −0.502 | 174 | 173 | 0/15 | 0.405 / 0.459 ✓ |

**F4를 통계적으로 "해소"하려면 gamma ≈ 1.0 이상이 필요한데, 그 지점에서
PageRank 상위 15개 중 12개가 읽기 순서 80위 밖으로 밀려난다.** 즉 F4 요약통계를
만족시키는 것과 중요한 파일을 앞에 두는 것이 양립하지 않는다.

> ⚠ 한때 gamma 기본값을 1.5로 두었다가 0.2로 되돌렸다. 1.5는 **위 F4 통계에 직접
> 맞춘 값**이었고, 같은 통계로 "F4 해소"를 검증했으므로 순환 논증이었다.
> `bench/CONTRACT.md` §3.4(하이퍼파라미터는 train 내부 CV로만) 위반이기도 하다.
> gamma의 값은 Phase 4에서 학습 인스턴스 CV로 정한다.

`corr_cx`가 0으로 내려가지 않는 것은 결함이 아니다. `complexity_score`가
가중치의 0.35를 in/out degree에 쓰기 때문에 이해 비용 자체가 중요도와 양의
상관을 갖는다 — `corr(pagerank, complexity) = +0.230`,
`corr(bc, complexity) = +0.323`. ease 항이 없는 gamma=0에서도 +0.674다.
**`corr_cx ≈ 0`을 목표로 삼으면 gamma ≈ 1.0이 되어 F1을 다시 망가뜨린다.**

**이 충돌의 근원은 gamma가 아니라 읽기 방향이다.** bottom-up에서는
"중요한 것 먼저"와 "쉬운 것 먼저"가 같은 축의 양 끝을 가리키므로, 한쪽을
만족시키면 다른 쪽이 나빠진다. 두 신호가 직교하려면
`02-reading-sequence-diagnosis.md` §2의 3단 구조(목적 seed → PPR → 부분그래프
위상정렬)가 필요하고, 그건 Phase 2·3의 일이다.

부차적 요인: `complexity_score`는 가중치의 0.35를 inbound/outbound degree에
쓴다(`complexity_w_inbound` 0.20 + `complexity_w_outbound` 0.15). 그래서
complexity가 PageRank 중요도와 양의 상관을 갖고, `combined`가 degree를 한 번은
양(PR/BC)으로 한 번은 음(ease)으로 이중 계산한다. degree 항을 0으로 두면
F4 마진이 +0.054 → +0.149로 넓어지지만 F1은 개선되지 않았고,
`complexity_score`는 `bench/CONTRACT.md`의 공유 피처라 Phase 0에서는 손대지
않았다.

---

## 5. 사람이 결정할 것

1. **읽기 방향**: bottom-up 유지 / top-down 전환 / 모드 분리(온보딩 vs 작업)
2. 전환한다면 **PageRank 방향(`adj` → `radj`)도 함께 바꿀지** — §1-(3)
3. `gamma` 기본값 0.2 — "중요도가 순서를 지배하고 복잡도는 tie-break 수준으로만
   개입한다"는 설계 의도로 정한 값이다. 최종 값은 Phase 4에서 NDCG/Recall로
   학습해 정한다. 진단 통계에 맞추지 말 것.
4. `complexity_w_inbound` / `complexity_w_outbound`를 0으로 내려
   "이해 비용"에서 degree를 빼고 PR/BC에만 맡길지
