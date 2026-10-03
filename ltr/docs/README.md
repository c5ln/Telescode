# Learning-to-Rank documentation timeline

이 디렉토리는 Telescode 추천 알고리즘이 결정론적 읽기 순서에서
issue-conditioned Learning-to-Rank 모델로 발전한 과정을 시간순으로 정리한다.
각 문서의 숫자는 작성 당시 실험 조건의 결과다. 현재 배포 모델의 기준은
`07-cost-free-ranking.md`와 `../README.md`, 실제 계약은
`../models/lambdarank-v1/artifact.json`이다.

## 문서 흐름

| 순서 | 시기 | 문서 | 종류 | 핵심 내용 | 현재 역할 |
|---:|---|---|---|---|---|
| 1 | 2026-05 | [`01-reading-sequence-plan.md`](01-reading-sequence-plan.md) | 최초 구현 계획 | PageRank, Betweenness Centrality, SCC, 위상정렬을 결합한 기존 C++ 읽기 순서 추천기의 구조 | LTR 비교 대상인 deterministic baseline의 설계 기록 |
| 2 | 2026-08-21 | [`02-reading-sequence-diagnosis.md`](02-reading-sequence-diagnosis.md) | 문제 진단·학습 제안 | 기존 추천기의 F1~F4 실패 모드를 측정하고, 이슈/커밋과 변경 파일로 관련성 가중치를 학습하는 방향 제안 | LTR 실험의 출발점 |
| 3 | 2026-08-22 | [`03-reading-direction.md`](03-reading-direction.md) | 방향성 분석 | bottom-up 읽기 방향, PageRank 흐름, 위상정렬 및 복잡도 가중치의 충돌 분석 | 기존 C++ 순서 알고리즘의 해석 근거 |
| 4 | 2026-08 이후 | [`04-data-evaluation-contract.md`](04-data-evaluation-contract.md) | 데이터·평가 계약 | SWE-bench 라벨, 피처 스키마, 누출 방지, 분할, 지표, 파이프라인 정의 | 모든 학습·평가가 따라야 하는 공통 계약 |
| 5 | 2026-08-24~25 | [`05-hybrid-ltr-results.md`](05-hybrid-ltr-results.md) | 전체 실험 보고서 | BM25, 임베딩, 리랭커, PPR, Hybrid LTR 및 ablation 결과. 외부 API 포함 최고 MRR 0.753 | 연구 결과와 기각된 가설의 이력; 현재 무료 모델과 구성은 다름 |
| 6 | 2026-09-13 | [`06-structure-aware-ai-direction.md`](06-structure-aware-ai-direction.md) | 후속 제품 방향 | 구조 신호의 역할을 수정 파일 순위에서 코드 설명·호출 흐름·UI 연동 근거로 확장하는 제안 | 후속 설계이며 아직 구현되지 않음 |
| 7 | 2026-09-15 | [`07-cost-free-ranking.md`](07-cost-free-ranking.md) | 무료 모델 선택 보고서 | 코드 어휘 피처, weighted sum, non-overlap 분석과 LambdaMART ablation. 19개 피처 base 모델 MRR 0.6488 선택 근거 | 현재 `lambdarank-v1`의 직접적인 실험 근거 |
| 8 | 2026-09-19 이후 | [`08-production-model-contract.md`](08-production-model-contract.md) | 제품 모델 계약 | 최종 학습·저장·로드·추론 책임, `model.txt`와 `artifact.json`, C++ LightGBM 연동 원칙 | 현재 제품화 인터페이스 |
| 9 | 2026-10-03 | [`09-lambdarank-v1-artifact.md`](09-lambdarank-v1-artifact.md) | 릴리스 모델 계약 | 19개 피처 순서, query별 min-max, `all_no_init`, LambdaRank 하이퍼파라미터 | C++ 구현이 검증해야 하는 최종 실행 계약 |

## 현재 기준과 역사적 결과 구분

- `bench/RESULTS.md`의 MRR 0.753은 임베딩과 외부 크로스 인코더를 포함한
  연구용 Hybrid LTR 결과다.
- 현재 생성된 `lambdarank-v1`은 외부 API 없이 재현 가능한 19개 피처 모델이며,
  선택 근거는 `07-cost-free-ranking.md`의 MRR 0.6488 결과다.
- `01`~`03`은 기존 PageRank/BC 추천기의 설계·진단 기록이며 현재 모델 계약이 아니다.
- C++ 추론 구현에서 이름이나 설명보다 우선하는 단일 기준은
  `artifact.json`의 피처 순서와 전처리 설정이다.
