# Hybrid LTR 전체 실험 결과

BM25에서 시작해 임베딩, Personalized PageRank, 외부 크로스 인코더 리랭킹과
LambdaRank를 차례로 비교한 연구 단계다.

- 데이터: 229개 SWE-bench 인스턴스, gold 437개
- 방어 조건: 이슈에서 파일 경로를 제거한 `no_paths`
- 최고 연구 성능: Hybrid LTR MRR 0.753, R@10 0.885
- 핵심 관찰: 임베딩과 BM25는 실패 구간이 달라 상보적이었다.
- 기각된 가설: 강한 의미 피처가 있을 때 graph 그룹의 한계 기여는 없거나 음수였다.
- 제약: 임베딩과 리랭커가 외부 API에 의존하므로 현재 무료 제품 모델과 다르다.

세부 조건, ablation, 유의성 검정 및 재현 명령의 단일 원본은
[`../../bench/RESULTS.md`](../../bench/RESULTS.md)다.
