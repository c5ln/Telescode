"""pytest 수집 범위 격리.

`bench/repos/` 에는 벤치마크 대상 저장소(xarray, pytest)를 clone 해 둔다.
그 안에 각 프로젝트의 자체 테스트가 들어 있어서, 격리하지 않으면
`pytest bench/` 가 남의 테스트 수천 개를 수집하려다 collection error 로 죽는다.
"""

collect_ignore_glob = ["repos/*", ".venv/*", "scratch/*", "data/*"]
