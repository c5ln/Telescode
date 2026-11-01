#!/bin/bash
# 이름 해석 개편 후 재측정 — 실제 실행 스크립트 (2026-11-01, 그대로 보관). 레포 루트 기준으로 돈다. API 호출 없음.
# phase1 다음에 실행. 원래 'ETOPK=2 EPOW=1 BTOPK=2 BPOW=1 phase2.sh'로 돌렸다 — 기본값을 그 값으로 고정했다.
# 주의: ppr_mechanism 줄은 이후 같은 인자로 다시 돌려 ppr_rank1_is 필드를 추가했다 (bench/data/resolver_fix/ppr_mechanism.json).
set -uo pipefail
cd "$(dirname "$0")/../../.."
PY=bench/.venv/bin/python
D=bench/data/resolver_fix
O=bench/data
SA=bench/data/seed_all
mkdir -p $D/seed_all
ADD="--add-repo pydata/xarray=bench/repos/xarray --add-repo pytest-dev/pytest=bench/repos/pytest"
echo "=== splits diff"; for f in splits_all splits_all_embedppr; do cmp $O/$f.csv $D/$f.csv && echo "$f identical"; done
cmp $D/splits_all.csv $O/splits_all_embedppr.csv && echo "splits_all(s6) == splits_all_embedppr(old)"
echo "=== edges $(date)"
$PY -m bench.features.edges --features $D/features_all_embedppr.csv --manifest $D/instances_all_embedppr.csv $ADD --out $D/file_edges_all.csv > $D/edges.log 2>&1; echo "edges rc=$?"; tail -2 $D/edges.log
echo "=== bm25 seeds $(date)"
$PY -m bench.seed.run_seed $ADD --features-csv $D/features_all_embedppr.csv --out-dir $D/seed_all > $D/run_seed.log 2>&1; echo "run_seed rc=$?"; tail -3 $D/run_seed.log
for f in bm25_seed__full bm25_seed__no_paths bm25_seed__no_trace bm25_seed_long bm25_leakage; do cmp $SA/$f.csv $D/seed_all/$f.csv && echo "$f byte-identical" || echo "$f DIFFERS"; done
echo "=== vocab $(date)"
$PY -m bench.seed.run_vocab $ADD --data-dir $D/seed_all --features-csv $D/features_all_embedppr.csv --seed-long $D/seed_all/bm25_seed_long.csv --sweep > $D/run_vocab.log 2>&1; echo "vocab rc=$?"
for f in vocab_overlap vocab_segment_eval vocab_definition_sweep; do cmp $SA/$f.csv $D/seed_all/$f.csv && echo "$f byte-identical" || echo "$f DIFFERS"; done
cmp $O/vocab_overlap.csv $D/seed_all/vocab_overlap.csv && echo "vocab_overlap == data/vocab_overlap.csv"
echo "=== join_bm25 / analyze_embed $(date)"
$PY -m bench.features.join_bm25 --features $D/features_all.csv --seed $SA/bm25_seed__no_paths.csv --out $D/features_all_with_bm25__no_paths.csv > $D/join_bm25.log 2>&1; echo "join rc=$?"
$PY -m bench.seed.analyze_embed --seed $SA/embed_seed__no_paths.csv --features $D/features_all_with_bm25__no_paths.csv > $D/analyze_embed.log 2>&1; echo "analyze rc=$?"
$PY -m bench.seed.analyze_embed --seed $SA/embed_seed__no_paths.csv --features $O/features_all_with_bm25__no_paths.csv > $D/analyze_embed_old.log 2>&1
echo "=== mechanism $(date)"
$PY -m bench.tools.ppr_mechanism --embed-ppr $D/features_all_embedppr.csv --bm25-ppr $D/features_all.csv --embed-seed $SA/embed_seed__no_paths.csv --bm25-seed $SA/bm25_seed__no_paths.csv --seed-topk ${ETOPK:-2} --seed-power ${EPOW:-1} --bm25-seed-topk ${BTOPK:-2} --bm25-seed-power ${BPOW:-1} --out $D/ppr_mechanism.json > /dev/null; echo "mech rc=$?"
COMMON="--include-init --bm25-seed $SA/bm25_seed__no_paths.csv --overlap $O/vocab_overlap.csv --overlap-condition no_paths --splits $D/splits_all.csv"
echo "=== LTR BM25 단계 $(date)"
$PY -m bench.ltr.run_ltr --features $D/features_all.csv --manifest $D/instances_all.csv $COMMON --out-prefix $D/ltr_ppr_nopaths > $D/ltr_ppr_nopaths.log 2>&1; echo rc=$?
echo "=== LTR 임베딩 단계 $(date)"
$PY -m bench.ltr.run_ltr --features $D/features_all_embedppr.csv --manifest $D/instances_all_embedppr.csv $COMMON --embed-seed $SA/embed_seed__no_paths.csv --out-prefix $D/ltr_embed_seg > $D/ltr_embed_seg.log 2>&1; echo rc=$?
echo "=== LTR 리랭커 단계 $(date)"
$PY -m bench.ltr.run_ltr --features $D/features_all_embedppr.csv --manifest $D/instances_all_embedppr.csv $COMMON --embed-seed $SA/embed_seed__no_paths.csv --rerank-seed $SA/rerank_seed__no_paths.csv --out-prefix $D/ltr_rerank > $D/ltr_rerank.log 2>&1; echo rc=$?
echo "=== PHASE2 DONE $(date)"
