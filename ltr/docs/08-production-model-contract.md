# 제품 모델 아티팩트 계약

실험 코드를 제품 수명주기와 분리하고 최종 모델을 언어 중립적인 아티팩트로
저장하는 단계다.

- `model.txt`: LightGBM portable text model
- `artifact.json`: 피처 순서, 정규화, 후보 정책, 하이퍼파라미터
- Python: 최종 학습, 저장, 로드와 기준 추론
- C++: JSON 계약 검증 후 같은 전처리와 피처 순서로 LightGBM C API 호출

제품 인터페이스의 단일 원본은 [`../README.md`](../README.md)다.
