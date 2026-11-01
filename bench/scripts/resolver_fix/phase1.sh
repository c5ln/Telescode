#!/bin/bash
# 이름 해석 개편 후 재측정 — 실제 실행 스크립트 (2026-11-01, 그대로 보관). 레포 루트 기준으로 돈다. API 호출 없음.
# phase1 → (topk는 ppr_select 출력에서 자동으로 읽는다. 실행 결과: bm25·embed 모두 topk2_pow1)
set -euo pipefail
cd "$(dirname "$0")/../../.."
PY=bench/.venv/bin/python
D=bench/data/resolver_fix
mkdir -p $D
GRID="0:1,0:2,0:3,50:1,20:1,10:1,5:1,20:2,10:2,1:1,2:1,3:1,3:2,5:2"
REPOS="--repo pydata/xarray --repo-dir bench/repos/xarray --repo pytest-dev/pytest --repo-dir bench/repos/pytest"
declare -A SEED=([bm25]=bench/data/seed_all/bm25_seed__no_paths.csv [embed]=bench/data/seed_all/embed_seed__no_paths.csv)
for s in bm25 embed; do
  echo "=== sweep $s $(date)"
  $PY -m bench.features.ppr_sweep $REPOS --seed ${SEED[$s]} --seed-weight-col $s \
     --splits bench/data/splits_all.csv --grid $GRID --out $D/ppr_sweep_${s}14.csv > $D/ppr_sweep_${s}14.log 2>&1
  $PY -m bench.features.ppr_select --sweep $D/ppr_sweep_${s}14.csv --splits bench/data/splits_all.csv \
     --manifest bench/data/instances_all.csv > $D/ppr_select_${s}.log 2>&1
  cfg=$(grep '^선택:' $D/ppr_select_${s}.log | awk '{print $2}')
  topk=$(echo $cfg | sed -E 's/topk([0-9]+)_pow.*/\1/'); pw=$(echo $cfg | sed -E 's/.*_pow(.*)/\1/')
  echo "selected $s: $cfg topk=$topk pow=$pw"
  suf=$([ $s = embed ] && echo _embedppr || echo "")
  for spec in pydata/xarray:xarray pytest-dev/pytest:pytest; do
    repo=${spec%%:*}; name=${spec##*:}
    echo "=== pipeline $s $name $(date)"
    $PY -m bench.collect.pipeline --repo $repo --repo-dir bench/repos/$name --split test \
      --ppr-bin pin --seed-csv ${SEED[$s]} --seed-weight-col $s --seed-topk $topk --seed-power $pw \
      --out $D/features_${name}${suf}.csv --manifest $D/instances_${name}${suf}.csv > $D/pipeline_${s}_${name}.log 2>&1
    tail -1 $D/pipeline_${s}_${name}.log
  done
  $PY -m bench.features.combine --features $D/features_xarray${suf}.csv $D/features_pytest${suf}.csv \
    --manifests $D/instances_xarray${suf}.csv $D/instances_pytest${suf}.csv \
    --out $D/features_all${suf}.csv --manifest-out $D/instances_all${suf}.csv --splits-out $D/splits_all${suf}.csv
done
echo "=== PHASE1 DONE $(date)"
