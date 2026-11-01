#!/bin/bash
# 개편 후 재측정 감사 후속 A: 그래프 없는 -graph (bench/tools/graphfree_ablation.py)
# + 옛 그래프 BM25 단계 run_ltr를 현재 코드·완전한 어휘 중첩 파일로 재실행.
# 실행: bash bench/scripts/resolver_fix/followup_graphfree.sh   (레포 루트 기준, API 호출 없음)
set -uo pipefail
cd "$(dirname "$0")/../../.."
PY=bench/.venv/bin/python; D=bench/data/resolver_fix; O=bench/data; SA=bench/data/seed_all
SEM="--embed-seed $SA/embed_seed__no_paths.csv --rerank-seed $SA/rerank_seed__no_paths.csv"
OV="--overlap $O/vocab_overlap.csv --overlap-condition no_paths"
echo "=== graph-free rerank stage, new graph $(date)"
$PY -m bench.tools.graphfree_ablation --features $D/features_all_embedppr.csv --manifest $D/instances_all_embedppr.csv \
  --bm25-seed $SA/bm25_seed__no_paths.csv $SEM $OV --old-features $O/features_all_embedppr.csv \
  --out $D/graphfree_rerank.json > $D/graphfree_rerank.log 2>&1; echo rc=$?
echo "=== graph-free rerank stage, old graph $(date)"
$PY -m bench.tools.graphfree_ablation --features $O/features_all_embedppr.csv --manifest $O/instances_all_embedppr.csv \
  --bm25-seed $SA/bm25_seed__no_paths.csv $SEM $OV \
  --out $D/graphfree_rerank_e2501cb.json > $D/graphfree_rerank_e2501cb.log 2>&1; echo rc=$?
echo "=== graph-free BM25 stage, new graph $(date)"
$PY -m bench.tools.graphfree_ablation --features $D/features_all.csv --manifest $D/instances_all.csv \
  --bm25-seed $SA/bm25_seed__no_paths.csv $OV --old-features $O/features_all.csv \
  --out $D/graphfree_bm25.json > $D/graphfree_bm25.log 2>&1; echo rc=$?
echo "=== graph-free BM25 stage, old graph $(date)"
$PY -m bench.tools.graphfree_ablation --features $O/features_all.csv --manifest $O/instances_all.csv \
  --bm25-seed $SA/bm25_seed__no_paths.csv $OV \
  --out $D/graphfree_bm25_e2501cb.json > $D/graphfree_bm25_e2501cb.log 2>&1; echo rc=$?
echo "=== old-graph BM25 stage run_ltr, current code, full overlap $(date)"
mkdir -p $D/e2501cb_rerun
$PY -m bench.ltr.run_ltr --include-init --features $O/features_all.csv --manifest $O/instances_all.csv \
  --bm25-seed $SA/bm25_seed__no_paths.csv $OV --splits $O/splits_all.csv \
  --out-prefix $D/e2501cb_rerun/ltr_ppr_nopaths > $D/e2501cb_rerun/ltr_ppr_nopaths.log 2>&1; echo rc=$?
echo "=== DONE $(date)"
