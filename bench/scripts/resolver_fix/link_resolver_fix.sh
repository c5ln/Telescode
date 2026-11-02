#!/bin/bash
# 이름 해석 개편 후 재측정 — 실제 실행: 단독 스캔 링크 품질 (bench/results/link_resolver_fix/). 레포 루트 기준.
set -euo pipefail
cd "$(dirname "$0")/../../.."
H=bench/bin/93f9cf2/TelescodeHeadless; TMP=$(mktemp -d)
for r in pytest xarray; do
  $H scan bench/repos/$r "$TMP/$r.db" >/dev/null; $H algo "$TMP/$r.db" >/dev/null
  python3 -m bench.tools.measure_links --db "$TMP/$r.db" --label $r --scanned-path bench/repos/$r \
    --binary $H --out bench/results/link_resolver_fix/$r.json
done
rm -rf -- "${TMP:?}"
