# Parser 벤치마크 — 병렬화 사전 측정 (baseline)

- 날짜: 2026-07-11
- 목적: Parser 병렬화 착수 전, 전체 스캔 시간에서 "파싱 vs DB insert" 비중 측정
- 환경: Linux, 12코어, gcc/g++ -O2 (Debug 빌드는 수치가 왜곡되므로 별도 컴파일)
- 측정 방법: `scan_main.cpp`의 단계별 타이머 출력(`[scan] Timing: parse X ms, db-insert Y ms`),
  페이지 캐시 워밍 후 각 3회 실행

## 결과

| 대상                         | 파일 수 | 파싱                   | DB insert          | 파싱 비중 |
|------------------------------|---------|------------------------|--------------------|-----------|
| /usr/lib/python3.11 (stdlib) | 680     | ~2,220 ms (2190–2241) | ~900 ms (890–926) | 71%       |
| repos/sherlock               | 16      | ~18 ms                 | ~10 ms             | 64%       |

- 파일당 평균 파싱 시간: ~3.3 ms
- 3회 측정 편차: 파싱 ±2% 이내로 안정적

## 해석

- 파싱이 명확한 지배 병목. 파일 간 독립이므로 12코어 병렬화 시
  파싱 단계는 이론상 ~200–300 ms까지 단축 가능.
- 전체 기준 기대치: 3.1초 → 약 1.1–1.2초 (**~2.6–2.8배**).
- 병렬화 후 새 병목은 DB insert(~0.9초)가 되나, 이미 단일 트랜잭션 + WAL +
  `synchronous=NORMAL`을 사용 중이라 추가 개선 여지는 크지 않음.
- 결론: 파서 병렬화 진행 근거 확인됨.

## 재현 방법

```sh
scripts/bench_parser.sh                  # 기본: /usr/lib/python3.11, 3회
scripts/bench_parser.sh <대상경로> <횟수>  # 예: scripts/bench_parser.sh repos/sherlock 5
```

기존 `build/`는 Debug 빌드라 측정이 왜곡되므로, 스크립트가 fetch된 소스를 재사용해
-O2 바이너리를 `build/bench/`에 따로 빌드한 뒤 캐시 워밍 후 반복 측정한다.
(사전 조건: CMake 빌드를 한 번 이상 실행해 `build/_deps/`가 존재해야 함)

병렬화 후에도 동일한 방법으로 측정하여 이 baseline과 비교할 것.
