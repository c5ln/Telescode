# lambdarank-v1 릴리스 아티팩트

2026-10-03에 무료 19개 피처 구성으로 전체 학습 데이터를 다시 학습해 첫 제품
아티팩트를 생성했다.

- 학습 인스턴스: 229
- 후보 정책: `all_no_init`
- 전처리: query별 min-max, 원래 결측값은 NaN 유지
- 모델: LightGBM LambdaRank, 15 leaves, learning rate 0.05, 300 trees
- 모델 파일: [`../models/lambdarank-v1/model.txt`](../models/lambdarank-v1/model.txt)
- 실행 계약: [`../models/lambdarank-v1/artifact.json`](../models/lambdarank-v1/artifact.json)

C++ 추론은 이 문서의 요약이 아니라 `artifact.json`의 피처 배열과 설정을 직접
검증해야 한다.
