#!/bin/bash
# 이름 해석 개편 후 재측정 — 실제 실행 스크립트 (2026-11-01, 그대로 보관). 레포 루트 기준으로 돈다. API 호출 없음.
# 민감도: 새 그래프 + 옛 config(topk1). phase1과 병렬로 돌렸다 (scratch DB 이름이 겹치지 않는 시점).
cd "$(dirname "$0")/../../.."
PY=bench/.venv/bin/python; D=bench/data/resolver_fix/sens_topk1
for spec in pydata/xarray:xarray pytest-dev/pytest:pytest; do repo=${spec%%:*}; name=${spec##*:}
$PY -m bench.collect.pipeline --repo $repo --repo-dir bench/repos/$name --split test --ppr-bin pin --seed-csv bench/data/seed_all/embed_seed__no_paths.csv --seed-weight-col embed --seed-topk 1 --seed-power 1.0 --out $D/features_${name}_embedppr.csv --manifest $D/instances_${name}_embedppr.csv > $D/pipeline_$name.log 2>&1; tail -1 $D/pipeline_$name.log; done
$PY -m bench.features.combine --features $D/features_xarray_embedppr.csv $D/features_pytest_embedppr.csv --manifests $D/instances_xarray_embedppr.csv $D/instances_pytest_embedppr.csv --out $D/features_all_embedppr.csv --manifest-out $D/instances_all_embedppr.csv --splits-out $D/splits_all.csv
SA=bench/data/seed_all
$PY -m bench.ltr.run_ltr --include-init --features $D/features_all_embedppr.csv --manifest $D/instances_all_embedppr.csv --bm25-seed $SA/bm25_seed__no_paths.csv --embed-seed $SA/embed_seed__no_paths.csv --rerank-seed $SA/rerank_seed__no_paths.csv --overlap bench/data/vocab_overlap.csv --overlap-condition no_paths --splits $D/splits_all.csv --out-prefix $D/ltr_rerank > $D/ltr_rerank.log 2>&1; echo ltr rc=$?
echo SENS DONE
