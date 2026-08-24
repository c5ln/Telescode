"""어휘 중첩 구간 산출 + 구간별 BM25 지표.

    python -m bench.seed.run_vocab --repo-dir <clone>

산출:
    bench/data/vocab_overlap.csv       # instance_id, file_id, condition, 플래그
    bench/data/vocab_segment_eval.csv  # 구간별 BM25 지표

지표는 `bench.metrics.ranking`을 그대로 쓴다. 구간 분리는 **gold 집합을 쪼개서**
한다 — 후보 랭킹은 전체를 유지하고, "이 구간의 gold를 얼마나 잘 올렸는가"를 본다.
후보를 쪼개면 순위가 바뀌어 다른 것을 재게 된다.

한 구간의 gold가 빈 인스턴스는 지표가 NaN이 되고 `nanmean`에서 빠진다
(계약: gold가 비면 0.0이 아니라 NaN).
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import pandas as pd

from bench.metrics.ranking import evaluate_instance
from bench.schema import C, DATA_DIR
from bench.seed import textproc
from bench.seed.dataset import gold_by_instance, load_instances
from bench.seed.evaluate import DEFAULT_KS, scanned_sets
from bench.seed.tokenizer import tokenize
from bench.seed.vocab import (COL_N_OVERLAP, COL_OVERLAP, COL_OVERLAP_TOKENS,
                              MAX_DF_RATIO, MIN_OVERLAP_TOKENS,
                              build_identifier_index, overlap_for_instance)

SEGMENTS = ("overlap", "non_overlap")


def build_indices(repo_dirs: dict[str, Path], features: pd.DataFrame,
                  *, include_module_names: bool = False) -> tuple[dict, dict]:
    """인스턴스별 식별자 인덱스를 한 번만 만든다.

    ast 파싱이 이 분석의 병목이라(xarray는 8천 줄짜리 파일이 있다) 임계값을
    흔들 때마다 다시 파싱하면 민감도 확인이 불가능해진다. 인덱스는 임계값과
    무관하므로 재사용한다.
    """
    cand = scanned_sets(features)
    ids = list(features[C.instance_id].unique())
    idxs, meta = {}, {"parse_failed_files": 0}
    for inst in load_instances(instance_ids=ids):
        if inst.instance_id not in cand:
            continue
        repo_dir = repo_dirs.get(inst.repo)
        if repo_dir is None:
            raise SystemExit(
                f"{inst.repo} 의 clone 경로가 없다. --add-repo {inst.repo}=<dir> 를 준다"
            )
        idx = build_identifier_index(repo_dir, inst.instance_id, inst.base_commit,
                                     candidate_file_ids=cand[inst.instance_id],
                                     include_module_names=include_module_names)
        meta["parse_failed_files"] += len(idx.parse_failed)
        idxs[inst.instance_id] = idx
    meta["n_instances"] = len(idxs)
    return idxs, meta


def compute_overlap(indices: dict,
                    *, conditions=textproc.CONDITIONS,
                    min_tokens: int = MIN_OVERLAP_TOKENS,
                    max_df_ratio: float = MAX_DF_RATIO) -> pd.DataFrame:
    instances = load_instances(instance_ids=list(indices))
    gold = {i.instance_id: set(i.gold_files) for i in instances}

    rows = []
    for inst in instances:
        idx = indices[inst.instance_id]
        for cond in conditions:
            q = set(tokenize(textproc.make_query(inst.problem_statement, cond)))
            flags = overlap_for_instance(idx, q, min_tokens=min_tokens,
                                         max_df_ratio=max_df_ratio)
            g = gold.get(inst.instance_id, set())
            for fid, (ov, n, toks) in flags.items():
                rows.append({C.instance_id: inst.instance_id, C.file_id: fid,
                             "condition": cond,
                             C.is_positive: int(fid in g),
                             COL_OVERLAP: int(ov), COL_N_OVERLAP: n,
                             COL_OVERLAP_TOKENS: ";".join(toks[:20])})
    return pd.DataFrame(rows)


def segment_gold(ov_df: pd.DataFrame) -> dict[str, dict[str, set[str]]]:
    """중첩 플래그 → {segment: {instance_id: gold 집합}}.

    gold 행만 남겨 구간을 만든다. 스캔에 없는 gold는 행 자체가 없어 어느 구간에도
    들어가지 않으므로 `unclassified`로 따로 센다 — 조용히 사라지면 두 구간의
    gold 합이 전체와 안 맞는데 아무도 눈치채지 못한다.
    """
    gpos = ov_df[ov_df[C.is_positive] == 1]
    return {
        "overlap": gpos[gpos[COL_OVERLAP] == 1].groupby(C.instance_id)[C.file_id].apply(set).to_dict(),
        "non_overlap": gpos[gpos[COL_OVERLAP] == 0].groupby(C.instance_id)[C.file_id].apply(set).to_dict(),
    }


def segment_eval_scores(scores: pd.DataFrame, score_col: str,
                        seg_gold: dict[str, dict[str, set[str]]],
                        features: pd.DataFrame,
                        *, higher_is_better: bool = True,
                        ks: tuple[int, ...] = DEFAULT_KS,
                        label: str | None = None) -> pd.DataFrame:
    """**임의 점수 컬럼**의 구간별 지표.

    BM25만이 아니라 `pagerank`, `bc`, `combined`, `ppr` 등 어떤 랭킹에도 쓴다.
    그래프 재스캔이 끝나면 같은 구간 정의로 즉시 비교가 나오게 하려는 것이다.

    Args:
        higher_is_better: `file_rank`처럼 **작을수록 좋은** 컬럼이면 False.
            부호를 뒤집지 않으면 랭킹이 정확히 거꾸로 나오는데, 지표는
            그럴듯한 숫자를 내놓아 버그가 드러나지 않는다.
    """
    scanned = scanned_sets(features)
    sign = 1.0 if higher_is_better else -1.0
    out = []
    for seg in SEGMENTS:
        recs = []
        for iid, g in scores.groupby(C.instance_id):
            sub = seg_gold[seg].get(iid, set())
            if not sub:
                continue  # 이 구간의 gold가 없는 인스턴스는 제외 (NaN 오염 방지)
            vals = [sign * float(v) for v in g[score_col]]
            recs.append(evaluate_instance(g[C.file_id].tolist(), vals, sub,
                                          scanned_file_ids=scanned[iid], ks=ks))
        if not recs:
            continue
        r = pd.DataFrame(recs)
        row = {"scorer": label or score_col, "segment": seg,
               "n_instances": len(recs),
               "n_gold": int(sum(len(v) for v in seg_gold[seg].values()))}
        for c in ["mrr", "reachable_recall_ceiling"] + \
                 [f"recall@{k}" for k in ks] + [f"ndcg@{k}" for k in ks]:
            row[c] = float(np.nanmean(r[c]))
        out.append(row)
    return pd.DataFrame(out)


def segment_eval(long_df: pd.DataFrame, ov_df: pd.DataFrame,
                 features: pd.DataFrame,
                 *, ks: tuple[int, ...] = DEFAULT_KS) -> pd.DataFrame:
    """구간별 BM25 지표. gold를 구간으로 쪼개고 후보 랭킹은 그대로 둔다."""
    scanned = scanned_sets(features)
    gold_all = {k: set(v) for k, v in
                gold_by_instance(features[C.instance_id].unique()).items()}

    out = []
    for cond in ov_df["condition"].unique():
        seed = long_df[long_df["condition"] == cond]
        ov = ov_df[ov_df["condition"] == cond]
        # gold 행만 남겨 구간을 만든다. 스캔에 없는 gold는 행이 없으므로
        # 어느 구간에도 안 들어간다 — 아래 unclassified로 별도 보고한다.
        gpos = ov[ov[C.is_positive] == 1]
        seg_gold = {
            "overlap": gpos[gpos[COL_OVERLAP] == 1].groupby(C.instance_id)[C.file_id].apply(set).to_dict(),
            "non_overlap": gpos[gpos[COL_OVERLAP] == 0].groupby(C.instance_id)[C.file_id].apply(set).to_dict(),
        }
        for seg in SEGMENTS:
            recs = []
            for iid, g in seed.groupby(C.instance_id):
                sub = seg_gold[seg].get(iid, set())
                if not sub:
                    continue  # 이 구간의 gold가 없는 인스턴스 — NaN 대신 제외
                recs.append(evaluate_instance(g[C.file_id].tolist(), g[C.bm25].tolist(),
                                              sub, scanned_file_ids=scanned[iid], ks=ks))
            if not recs:
                continue
            r = pd.DataFrame(recs)
            row = {"condition": cond, "segment": seg,
                   "n_instances": len(recs),
                   "n_gold": int(sum(len(seg_gold[seg].get(i, set()))
                                     for i in seed[C.instance_id].unique()))}
            for c in ["mrr", "reachable_recall_ceiling"] + \
                     [f"recall@{k}" for k in ks] + [f"ndcg@{k}" for k in ks]:
                row[c] = float(np.nanmean(r[c]))
            out.append(row)

        # 구간 배정이 안 된 gold (스캔에 아예 없는 파일)
        n_scan_gold = int(gpos[C.instance_id].map(lambda x: 1).sum())
        n_all_gold = sum(len(gold_all.get(i, set())) for i in seed[C.instance_id].unique())
        out.append({"condition": cond, "segment": "unclassified(스캔에 없음)",
                    "n_instances": 0, "n_gold": n_all_gold - n_scan_gold})
    return pd.DataFrame(out)


def sweep_definitions(repo_dirs: dict[str, Path], features: pd.DataFrame,
                      long_df: pd.DataFrame,
                      condition: str = textproc.NO_TRACE) -> pd.DataFrame:
    """임계값을 흔들어 구간 크기와 구간별 지표가 얼마나 요동치는지 본다.

    결론이 임계값 선택에 의존하면 그 결론은 쓸 수 없다. 그래서 구간 **크기**만이
    아니라 non_overlap 구간의 **BM25 지표**까지 같이 흔들어 본다 — 그래프가
    넘어야 할 바가 정의에 따라 달라지는지가 진짜 질문이기 때문이다.
    """
    rows = []
    for mod in (False, True):
        idxs, _ = build_indices(repo_dirs, features, include_module_names=mod)
        for min_tok in (1, 2, 3):
            for df_ratio in (1.00, 0.25, 0.10, 0.05):
                d = compute_overlap(idxs, conditions=(condition,),
                                    min_tokens=min_tok, max_df_ratio=df_ratio)
                g = d[d[C.is_positive] == 1]
                seg = segment_eval(long_df, d, features)
                nov = seg[seg.segment == "non_overlap"]
                rows.append({
                    "module_names": mod, "min_tokens": min_tok,
                    "max_df_ratio": df_ratio,
                    "n_gold_in_scan": len(g),
                    "n_non_overlap": int((g[COL_OVERLAP] == 0).sum()),
                    "pct_non_overlap": float((g[COL_OVERLAP] == 0).mean()),
                    "nonov_instances": int(nov["n_instances"].iloc[0]) if len(nov) else 0,
                    "nonov_mrr": float(nov["mrr"].iloc[0]) if len(nov) else float("nan"),
                    "nonov_r10": float(nov["recall@10"].iloc[0]) if len(nov) else float("nan"),
                })
    return pd.DataFrame(rows)


def main(argv=None):
    ap = argparse.ArgumentParser(description="어휘 중첩 구간 분석")
    ap.add_argument("--repo", default=None, help="단일 repo. --repo-dir와 함께")
    ap.add_argument("--repo-dir", type=Path, default=None)
    ap.add_argument("--add-repo", action="append", default=[], metavar="REPO=DIR",
                    help="여러 repo를 한 번에. 반복 지정 가능")
    ap.add_argument("--data-dir", type=Path, default=DATA_DIR)
    ap.add_argument("--features-csv", type=Path, default=None,
                    help="기본 <data-dir>/features.csv")
    ap.add_argument("--seed-long", type=Path, default=None,
                    help="기본 <data-dir>/bm25_seed_long.csv")
    ap.add_argument("--min-tokens", type=int, default=MIN_OVERLAP_TOKENS)
    ap.add_argument("--max-df-ratio", type=float, default=MAX_DF_RATIO)
    ap.add_argument("--sweep", action="store_true",
                    help="임계값 민감도. 결론이 정의에 의존하는지 확인한다")
    ap.add_argument("--compare", action="store_true",
                    help="구간별로 BM25와 그래프 피처를 나란히 비교 (재스캔 후 사용)")
    ap.add_argument("--compare-condition", default=textproc.NO_TRACE)
    a = ap.parse_args(argv)

    repo_dirs: dict[str, Path] = {}
    for spec in a.add_repo:
        if "=" not in spec:
            raise SystemExit(f"--add-repo 형식은 REPO=DIR 이다: {spec!r}")
        r, d = spec.split("=", 1)
        repo_dirs[r] = Path(d)
    if a.repo or a.repo_dir:
        if not (a.repo and a.repo_dir):
            raise SystemExit("--repo 와 --repo-dir 는 함께 준다")
        repo_dirs[a.repo] = a.repo_dir
    if not repo_dirs:
        raise SystemExit("--repo/--repo-dir 또는 --add-repo 중 하나는 필요하다")

    features = pd.read_csv(a.features_csv or (a.data_dir / "features.csv"))
    long_df = pd.read_csv(a.seed_long or (a.data_dir / "bm25_seed_long.csv"))

    idxs, meta = build_indices(repo_dirs, features)
    ov = compute_overlap(idxs, min_tokens=a.min_tokens,
                         max_df_ratio=a.max_df_ratio)
    ov.to_csv(a.data_dir / "vocab_overlap.csv", index=False)
    print(f"instances={meta['n_instances']}  ast 파싱 실패 파일={meta['parse_failed_files']}")
    print(f"vocab_overlap.csv  rows={len(ov)}  -> {a.data_dir}")

    seg = segment_eval(long_df, ov, features)
    seg.to_csv(a.data_dir / "vocab_segment_eval.csv", index=False)
    print()
    cols = ["condition", "segment", "n_instances", "n_gold", "mrr",
            "recall@10", "ndcg@10", "reachable_recall_ceiling"]
    print(seg[[c for c in cols if c in seg.columns]].to_string(index=False))

    if a.compare:
        # 재스캔 직후 결정적 비교를 바로 낼 수 있게 해두는 자리.
        # `file_rank`는 작을수록 좋으므로 부호를 뒤집는다.
        seg_g = segment_gold(ov[ov["condition"] == a.compare_condition])
        seed_c = long_df[long_df["condition"] == a.compare_condition][
            [C.instance_id, C.file_id, C.bm25]]
        parts = [segment_eval_scores(seed_c, C.bm25, seg_g, features,
                                     label=f"bm25({a.compare_condition})")]
        for col, hib in [(C.combined, True), (C.pagerank, True), (C.bc, True),
                         (C.ppr, True), (C.file_rank, False),
                         (C.complexity, True)]:
            if col not in features.columns or features[col].isna().all():
                print(f"  (건너뜀: {col} 비어 있음 — 재스캔/Phase 3 대기)")
                continue
            parts.append(segment_eval_scores(features, col, seg_g, features,
                                             higher_is_better=hib))
        cmp_df = pd.concat(parts, ignore_index=True)
        cmp_df.to_csv(a.data_dir / "vocab_segment_compare.csv", index=False)
        print(f"\n=== 구간별 스코어러 비교 (condition={a.compare_condition}) ===")
        print(cmp_df[["scorer", "segment", "n_instances", "n_gold",
                      "mrr", "recall@10", "ndcg@10"]].to_string(index=False))

    if a.sweep:
        print("\n=== 임계값 민감도 (condition=no_trace) ===")
        sw = sweep_definitions(repo_dirs, features, long_df)
        sw.to_csv(a.data_dir / "vocab_definition_sweep.csv", index=False)
        print(sw.to_string(index=False))


if __name__ == "__main__":
    main()
