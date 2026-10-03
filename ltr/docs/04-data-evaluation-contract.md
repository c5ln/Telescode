# 데이터·평가 계약

이 단계에서 추천 알고리즘 실험의 공통 규칙이 확정됐다.

- 데이터: SWE-bench의 xarray 110개와 pytest 119개 이슈
- query: 이슈의 `problem_statement`
- label: gold patch가 변경한 파일의 `is_positive=1`
- 피처 시점: 수정 전 `base_commit`
- 분할 단위: 파일이 아니라 `instance_id`
- 평가: MRR, Recall@K, NDCG@K와 reachable recall ceiling
- 튜닝: 외부 test가 아닌 train 내부 CV

피처 스키마, positive-unlabeled 주의사항, 누출 방지 규칙과 파이프라인의
단일 원본은 [`../../bench/CONTRACT.md`](../../bench/CONTRACT.md)다. 계약을 수정할
때는 이 요약을 복제해 고치지 말고 원본을 먼저 갱신한다.
