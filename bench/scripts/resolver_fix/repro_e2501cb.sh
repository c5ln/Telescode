#!/bin/bash
# 이름 해석 개편 후 재측정 — 실제 실행 (2026-11-01): 옛 pin과 현재 bench 코드로 §1~§12 수치가 재현되는지 확인.
# 레포 루트 기준. 출력은 OUT(기본 scratch 밖 임시 폴더). API 호출 없음.
set -uo pipefail
cd "$(dirname "$0")/../../.."
PY=bench/.venv/bin/python; OUT=${OUT:-$(mktemp -d)}; SA=bench/data/seed_all
# 1. 옛 pin으로 피처 재생성 (3개씩) → features_*_embedppr.csv / features_all.csv와 비교
TELESCODE_BENCH_PIN=e2501cb $PY -m bench.collect.pipeline --repo pydata/xarray --repo-dir bench/repos/xarray \
  --split test --limit 3 --ppr-bin pin --seed-csv $SA/embed_seed__no_paths.csv --seed-weight-col embed \
  --seed-topk 1 --seed-power 1.0 --out $OUT/try_e2501cb.csv --manifest $OUT/try_e2501cb_man.csv
TELESCODE_BENCH_PIN=e2501cb $PY -m bench.collect.pipeline --repo pytest-dev/pytest --repo-dir bench/repos/pytest \
  --split test --limit 3 --ppr-bin pin --seed-csv $SA/bm25_seed__no_paths.csv --seed-weight-col bm25 \
  --seed-topk 1 --seed-power 1.0 --out $OUT/try_bm25.csv --manifest $OUT/try_bm25_man.csv
# 2. 옛 CSV로 최종 LTR (--include-init: 60c4d04 이전 후보 집합) → bench/data/ltr_rerank_*와 cmp
$PY -m bench.ltr.run_ltr --include-init --features bench/data/features_all_embedppr.csv \
  --manifest bench/data/instances_all_embedppr.csv --bm25-seed $SA/bm25_seed__no_paths.csv \
  --embed-seed $SA/embed_seed__no_paths.csv --rerank-seed $SA/rerank_seed__no_paths.csv \
  --overlap bench/data/vocab_overlap.csv --overlap-condition no_paths --splits bench/data/splits_all.csv \
  --out-prefix $OUT/ltr_rerank_init > $OUT/ltr_rerank_init.log 2>&1
for f in ablation baselines fusion holdout size_control gain size_pairs; do
  cmp bench/data/ltr_rerank_$f.csv $OUT/ltr_rerank_init_$f.csv && echo "$f identical"; done
echo "outputs in $OUT"
