# 스캔 저장·해석 성능 최적화 검증

2026-11-02. 기준선은 `797601d`, 비교 대상은 현재 작업 트리의 DB v8 구현이다.
정수 키 전환 없이 CHECK 제약의 IN을 OR로 바꾸고, 다섯 테이블을 WITHOUT ROWID로
저장하며, 보조 인덱스를 일괄 생성하고, 참조를 해석 결과와 함께 한 번만 쓴다.
`raw_text`, `owner_id` 컬럼은 유지했다. 커밋은 하지 않았다.

## 측정 조건

- Release, 대상마다 새 DB로 스캔 3회. 스캔 시간은 프로세스 전체 wall time의 최솟값.
- DB 크기는 **scan 직후, algo 실행 전**이며 십진 MB다. RSS는 3회 중 최대, MiB다.
- 기준선과 변경본 측정은 순차 실행했다. 시간 원시값은 `scan_timing.json`에 있다.
- xarray 241개, pytest 271개, 루트 4,659개 Python 파일. 루트에는 기존과 같이
  `bench/.venv`와 하위 저장소가 포함된다.
- 바이너리 지문, 소스 지문, 비교 범위는 `validation.json`에 있다.

| 대상 | 스캔 전 → 후 (초) | DB 전 → 후 (MB) | 최대 RSS 전 → 후 (MiB) |
|---|---:|---:|---:|
| xarray | 3.130 → 1.484 | 60.1 → 38.4 | 284 → 256 |
| pytest | 1.374 → 0.745 | 32.9 → 21.1 | 144 → 129 |
| 루트 | 54.721 → 17.555 | 1,047.8 → 667.6 | 1,875 → 1,879 |

목표 네 개(xarray 1.6초·40 MB, 루트 25초·700 MB)를 모두 충족한다.
루트 스캔은 약 3.1배 빨라졌고 DB는 36.3% 줄었다. 루트 최대 RSS 차이는
4 MiB(0.2%)로 사실상 같은 수준이며, 메모리 감소를 달성했다고 주장하지 않는다.
이전 중간 보고의 시간과 이번 측정값을 섞지 않고 이번 환경의 기준선을 함께 쟀다.

## 정확성

- `ctest --test-dir build -E LayoutTests --output-on-failure`: 6개 타깃,
  **363개 테스트 통과**. CHECK 허용·거부값, 중복 참조, 재스캔 설정 보존,
  스캔 실패 및 SIGKILL 복구 테스트를 포함한다.
- fixtures, sherlock, pytest, xarray, 루트에서 두 바이너리로 각각 `scan` + `algo`를
  실행해 `bench.tools.update_equiv.compare_dbs`로 비교했다. **다섯 대상 모두 동일**.
  link set/multiset, reading_sequence의 점수 비트, 의존 기록, 참조 순서까지 동일하다.
  의도적으로 달라진 schema와 user_version, 실행 시각은 제외한다.
- update 비교 **50/50 통과**, 시간 목표가 있는 시나리오도 모두 통과했다.
  `update_equiv_timing.json`은 앞서 완료된 3회 측정 원본이다. 기록된 바이너리 MD5
  `6331bfe3afc159a21cd2b7f44e11b5f4`가 이번에 재빌드한 Release 바이너리와 같다.
  일반 xarray 수정 54.3 ms, dataset.py 218.4 ms, 루트 수정 22.4 ms.
- `bench/.venv/bin/python -m pytest bench -q`: **235개 통과**.
  pytest·xarray에서 실제 `extract` 출력도 24개 컬럼 전체를 `check_exact=True`로
  비교해 일치했다(`features.json`). 각각 271행·241행이다.
- xarray DB의 `integrity_check = ok`, `foreign_key_check` 결과 없음.
  검사에는 프로젝트에 포함된 SQLite 3.51.3을 사용했다. 시스템 Python의 SQLite
  3.40.1은 WITHOUT ROWID param 테이블에 실제로 없는 NULL 오류를 보고했다.
  같은 DB를 포함된 라이브러리로 검사해 정상임을 확인했다. 유사한 옛 버전 검사
  문제는 [SQLite 공식 포럼](https://sqlite.org/forum/info/c72b92be1e0bf341)에 있다.

## 조회 계획과 범위

`query_plans.json`에 실제 EXPLAIN QUERY PLAN 결과를 남겼다. 파일별 symbol/ref 조회,
scope/binding 조회, 상속 source/target 조회, dangling import 조회가 각각 보조 인덱스나
기본 키를 사용한다. dangling의 서브모듈 검색은 기존처럼 binding 전체를 조회한다.

기존 LayoutTests의 헤더 경로 문제는 이번 변경 범위 밖이므로 제외했다.
DB 형식은 v8로 바뀌었으므로 기존 v7 DB는 다시 scan해야 한다. bench 고정 바이너리와
기존 평가 데이터는 이번 단계에서 교체하지 않았다.

## 재현

Release에서 `TelescodeHeadless`를 빌드한 뒤 대상별로 다음을 3번 실행한다.
매 실행마다 **새 경로의 DB**를 사용하고, scan 종료 직후 크기를 기록한다.

```sh
/usr/bin/time -f 'elapsed=%e peak_rss_kib=%M' /path/to/Release/TelescodeHeadless scan /path/to/repo /tmp/fresh-run-1.db
/path/to/Release/TelescodeHeadless algo /tmp/fresh-run-1.db
```

정확성 검증에는 두 빌드의 scan+algo DB에 `compare_dbs`를 실행한다. update 비교는
다음 명령으로 재현한다. 시나리오마다 새 스캔 결과와 수정 후 결과를 비교한다.

```sh
bench/.venv/bin/python -m bench.tools.update_equiv --bin /path/to/Release/TelescodeHeadless --work /tmp/update-validation --timing --out /tmp/update-validation.json
```
