"""결정론적 결합 공식 — BM25(관련성)와 구조 점수(권위)를 섞는 네 가지 형태.

`run_ltr`의 fusion(RRF)은 두 순위에 같은 무게를 줘서 가중치를 조절할 수 없다.
여기서는 학습 없는 공식을 파라미터 1~2개로 흔들고, 제품(`reading_sequence_config`)에
그대로 넣을 수 있는 후보를 찾는다.

| 형태 | 점수 | 의도 |
|---|---|---|
| `linear`  | α·nb + (1−α)·np                                  | 덧셈. 무관한 허브도 권위 점수를 받는다 |
| `mult`    | nb · (ε + np)^β                                  | 관련성 × 권위 (웹 검색의 static rank prior) |
| `gate`    | (α₀ + (1−α₀)·pct) · nb                           | 중심적인 파일일수록 BM25를 더 믿는다 |
| `cascade` | BM25 상위 N개만 w·cb + (1−w)·cp, 나머지는 BM25 순 | 검색 → 재정렬 2단계 |

`nb`/`np`: 인스턴스 내 min-max 정규화한 bm25 / 구조 점수. `pct`: 구조 점수의 인스턴스 내
백분위. `cb`/`cp`: 후보 N개 안에서 다시 정규화한 값.

## 파라미터 선택 (무결성 규칙 §4)

outer fold의 **train 인스턴스 평균 MRR로만** 고르고 그 fold의 test에 적용한다.
fold는 `run_ltr`과 같은 `make_folds(..., SEED)`라 LambdaMART와 짝지은 비교가 된다.
그리드는 **BM25와 같은 설정이 맨 앞**이 되도록 정렬했고, 동점이면 앞쪽을 고른다 —
구조 점수를 섞을 근거가 train에서 안 보이면 섞지 않는다.

전 인스턴스 그리드 곡선은 **서술용**으로만 낸다. 거기서 최댓값을 골라 보고하면
test를 보고 고른 숫자가 된다.

    bench/.venv/bin/python -m bench.ltr.weighted_sum \
        --features bench/data/features_all_with_bm25__no_paths.csv \
        --manifest bench/data/instances_all.csv \
        --extra-features bench/data/code_lexical__no_paths.csv \
        --overlap bench/data/vocab_overlap.csv --overlap-condition no_paths \
        --out-prefix bench/data/wsum3_nopaths
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import numpy as np
import pandas as pd

from bench.features.code_lexical import EXTRA_GROUPS
from bench.ltr import cv as M
from bench.ltr.data import (FEATURE_GROUPS, load_dataset, per_instance_minmax,
                            restrict_candidates)
from bench.ltr.evaluate import evaluate_all, fmt, paired_bootstrap, per_instance_metrics
from bench.ltr.run_ltr import B2_COL, _print_tests, add_b2
from bench.schema import C, DATA_DIR

SCORE = "wsum"

# (라벨, 구조 점수 컬럼)
PARTNERS = {
    "B2": B2_COL,
    "combined": C.combined,
    "pagerank": C.pagerank,
    "bc": C.bc,
    "ppr": C.ppr,
}


@dataclass
class Ctx:
    """점수 계산에 필요한 인스턴스 내 정규화 값. 한 번 만들어 모든 그리드가 공유한다."""

    features: pd.DataFrame
    norm: pd.DataFrame        # bm25 + 구조 점수들, 인스턴스 내 min-max
    pct: pd.DataFrame         # 구조 점수들, 인스턴스 내 백분위
    bm25_rank: np.ndarray     # 인스턴스 내 내림차순 순위, 동점은 최선(min) — 후보 포함 판정용


def make_ctx(features: pd.DataFrame, partner_cols: list[str]) -> Ctx:
    norm = per_instance_minmax(features, [C.bm25] + partner_cols)
    g = features.groupby(C.instance_id)
    pct = pd.DataFrame({c: g[c].rank(pct=True) for c in partner_cols}, index=features.index)
    rank = g[C.bm25].rank(ascending=False, method="min").to_numpy()
    return Ctx(features=features, norm=norm, pct=pct, bm25_rank=rank)


def _linear(ctx: Ctx, col: str, p: dict) -> np.ndarray:
    a = p["alpha"]
    return a * ctx.norm[C.bm25].to_numpy() + (1.0 - a) * ctx.norm[col].to_numpy()


def _mult(ctx: Ctx, col: str, p: dict) -> np.ndarray:
    return ctx.norm[C.bm25].to_numpy() * (p["eps"] + ctx.norm[col].to_numpy()) ** p["beta"]


def _gate(ctx: Ctx, col: str, p: dict) -> np.ndarray:
    a0 = p["a0"]
    return (a0 + (1.0 - a0) * ctx.pct[col].to_numpy()) * ctx.norm[C.bm25].to_numpy()


def _cascade(ctx: Ctx, col: str, p: dict) -> np.ndarray:
    """상위 N 후보는 [0, 1] 안에서 재정렬, 나머지는 BM25 순서로 그 아래(−2 ~ −1)."""
    f = ctx.features
    cand = ctx.bm25_rank <= p["n"]
    tmp = pd.DataFrame({C.instance_id: f[C.instance_id].to_numpy(),
                        "b": np.where(cand, f[C.bm25].to_numpy(dtype=float), np.nan),
                        "p": np.where(cand, f[col].to_numpy(dtype=float), np.nan)})
    cn = per_instance_minmax(tmp, ["b", "p"])
    inner = p["w"] * cn["b"].to_numpy() + (1.0 - p["w"]) * cn["p"].to_numpy()
    return np.where(cand, inner, ctx.norm[C.bm25].to_numpy() - 2.0)


@dataclass(frozen=True)
class Form:
    key: str
    fn: Callable[[Ctx, str, dict], np.ndarray]
    grid: tuple[dict, ...]        # BM25와 같은 설정이 맨 앞
    partners: tuple[str, ...]


FORMS = [
    Form("linear", _linear,
         tuple({"alpha": round(a, 2)} for a in np.linspace(1.0, 0.0, 21)),
         ("B2", "combined", "pagerank", "bc", "ppr")),
    Form("mult", _mult,
         ({"beta": 0.0, "eps": 0.5},)
         + tuple({"beta": b, "eps": e} for b in (0.25, 0.5, 1.0, 2.0, 4.0)
                 for e in (0.5, 0.1, 0.01)),
         ("B2", "combined", "pagerank")),
    Form("gate", _gate,
         tuple({"a0": a} for a in (1.0, 0.9, 0.75, 0.5, 0.25, 0.0)),
         ("B2", "combined", "pagerank")),
    Form("cascade", _cascade,
         tuple({"n": n, "w": w} for w in (1.0, 0.9, 0.8, 0.7, 0.6, 0.5, 0.3, 0.0)
               for n in (5, 10, 20, 50)),
         ("B2", "combined", "pagerank")),
]


def score_frame(ctx: Ctx, form: Form, col: str, params: dict) -> pd.DataFrame:
    return pd.DataFrame({C.instance_id: ctx.features[C.instance_id].to_numpy(),
                         C.file_id: ctx.features[C.file_id].to_numpy(),
                         SCORE: form.fn(ctx, col, params)})


def select_params(mrr_tbl: pd.DataFrame, train_ids: list[str]) -> int:
    """train 인스턴스 평균 MRR이 최대인 그리드 인덱스. 동점이면 **앞쪽**(BM25 쪽).

    `mrr_tbl`: index = instance_id, columns = 그리드 인덱스(0..).
    """
    means = mrr_tbl.loc[train_ids].mean(axis=0)
    best = means.max()
    return int(min(i for i, v in means.items() if v >= best - 1e-12))


def _ltr_oof(ds, label: str, cols: list[str], folds, cache: Path | None) -> pd.DataFrame:
    """LambdaMART OOF 점수. 같은 피처·fold면 캐시를 쓴다 (학습에 수 분이 걸린다)."""
    if cache is not None:
        csv = cache.with_name(f"{cache.name}__{label}.csv")
        meta = csv.with_suffix(".json")
        want = {"columns": cols, "seed": M.SEED, "n_folds": folds.n_folds,
                "instances": len(ds.instance_ids), "candidate_rows": len(ds.features),
                "candidate_policy": "all_no_init"}
        if csv.exists() and meta.exists() and json.loads(meta.read_text()) == want:
            print(f"  LTR {label}: 캐시 사용 ({csv})")
            return pd.read_csv(csv)
    res = M.oof_predict(ds, cols, folds=folds, tune=True)
    if cache is not None:
        res.scores.to_csv(csv, index=False)
        meta.write_text(json.dumps(want, ensure_ascii=False))
    print(f"  LTR {label}: 피처 {len(cols)}개 학습 완료")
    return res.scores


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--features", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--extra-features", default=None,
                    help="주면 코드 어휘 피처 LambdaMART도 짝지은 비교에 넣는다")
    ap.add_argument("--overlap", default=str(DATA_DIR / "vocab_overlap.csv"))
    ap.add_argument("--overlap-condition", default="no_paths")
    ap.add_argument("--forms", default=",".join(f.key for f in FORMS))
    ap.add_argument("--skip-ltr", action="store_true", help="LambdaMART 짝 비교 생략")
    ap.add_argument("--ltr-cache", default=str(DATA_DIR / "ltr_oof_nopaths"),
                    help="LambdaMART OOF 점수 캐시 접두사. 빈 문자열이면 캐시 안 씀")
    ap.add_argument("--out-prefix", default=str(DATA_DIR / "wsum"))
    args = ap.parse_args(argv)

    ds = load_dataset(Path(args.features), Path(args.manifest),
                      bm25_seed_path=None,
                      extra_features_path=(Path(args.extra_features)
                                           if args.extra_features else None),
                      overlap_path=Path(args.overlap),
                      overlap_condition=args.overlap_condition)
    ds.features = add_b2(ds.features).reset_index(drop=True)
    features = ds.features
    forms = [f for f in FORMS if f.key in set(args.forms.split(","))]
    used = sorted({p for f in forms for p in f.partners})
    partner_cols = [PARTNERS[p] for p in used]
    for p, c in zip(used, partner_cols):
        if features[c].isna().any():
            raise SystemExit(f"구조 점수 {p}({c})에 결측이 있다")
    ctx = make_ctx(features, partner_cols)

    ids = ds.instance_ids
    folds = M.make_folds(ids, M.N_OUTER_FOLDS, M.SEED)
    print(f"인스턴스 {len(ids)} · fold {folds.n_folds} · 형태 {[f.key for f in forms]}")
    for n in ds.notes:
        print(f"  - {n}")

    rows, per, curve_rows, chosen = [], {}, [], {}

    for lab, col in [("bm25", C.bm25)] + [(p, PARTNERS[p]) for p in used]:
        r, pp = evaluate_all(ds, features[[C.instance_id, C.file_id, col]], col, lab)
        rows.extend(r)
        per[lab] = pp

    for form in forms:
        for partner in form.partners:
            col = PARTNERS[partner]
            mrr = {}
            for gi, params in enumerate(form.grid):
                m = per_instance_metrics(ds, score_frame(ctx, form, col, params), SCORE)
                mrr[gi] = m.set_index(C.instance_id)["mrr"]
                curve_rows.append({"form": form.key, "partner": partner,
                                   "params": json.dumps(params), "MRR": float(m["mrr"].mean()),
                                   "R@1": float(m["recall@1"].mean()),
                                   "R@10": float(m["recall@10"].mean())})
            mrr_tbl = pd.DataFrame(mrr)

            parts, picks = [], []
            for f in range(folds.n_folds):
                te = [i for i in ids if folds.fold_of(i) == f]
                tr = [i for i in ids if folds.fold_of(i) != f]
                params = form.grid[select_params(mrr_tbl, tr)]
                picks.append(params)
                s = score_frame(ctx, form, col, params)
                parts.append(s[s[C.instance_id].isin(te)])
            label = f"{form.key}(bm25, {partner})"
            chosen[label] = picks
            r, pp = evaluate_all(ds, pd.concat(parts, ignore_index=True), SCORE, label)
            rows.extend(r)
            per[label] = pp
            print(f"  {label:26s} MRR {r[0]['MRR']:.4f}  선택 {picks}", flush=True)

    if not args.skip_ltr:
        ltr_ds = restrict_candidates(ds, top_k=None, exclude_init=True)
        print(f"  - {ltr_ds.notes[-2]}")
        print(f"  - {ltr_ds.notes[-1]}")
        cache = Path(args.ltr_cache) if args.ltr_cache else None
        base_cols = [c for g in FEATURE_GROUPS for c in ltr_ds.groups_for(g)]
        cfgs = [("LTR base (무료 피처)", "base", base_cols)]
        extra_cols = [c for g in EXTRA_GROUPS for c in ltr_ds.groups_for(g)]
        if extra_cols:
            cfgs.append(("LTR +코드 어휘 피처", "codelex", base_cols + extra_cols))
        for lab, key, cols in cfgs:
            sc = _ltr_oof(ltr_ds, key, cols, folds, cache)
            r, pp = evaluate_all(ltr_ds, sc, "ltr_score", lab)
            rows.extend(r)
            per[lab] = pp

    tbl = pd.DataFrame(rows)
    curve = pd.DataFrame(curve_rows)

    for seg in ("all", "overlap", "non_overlap"):
        sub = tbl[tbl["segment"] == seg]
        if not sub.empty:
            print(f"\n── segment = {seg} ──")
            print(fmt(sub.drop(columns=["segment"]).sort_values("MRR", ascending=False)))

    ref_linear = "linear(bm25, combined)"
    tests = {}
    for label in [l for l in per if "(bm25, " in l]:
        refs = ["bm25"] + ([ref_linear] if label != ref_linear and ref_linear in per else []) \
            + [l for l in per if l.startswith("LTR")]
        for seg in ("all", "overlap", "non_overlap"):
            if seg not in per[label]:
                continue
            for ref in refs:
                if seg in per.get(ref, {}):
                    tests[f"{label} - {ref} [{seg}]"] = paired_bootstrap(
                        per[label][seg], per[ref][seg])
    print()
    _print_tests({k: v for k, v in tests.items() if k.endswith("[all]")})
    _print_tests({k: v for k, v in tests.items() if k.endswith("[non_overlap]")})

    pref = args.out_prefix
    tbl.to_csv(f"{pref}_results.csv", index=False)
    curve.to_csv(f"{pref}_curve.csv", index=False)
    with open(f"{pref}_tests.json", "w") as fh:
        json.dump({"tests": tests, "chosen": chosen,
                   "grids": {f.key: list(f.grid) for f in forms},
                   "notes": ds.notes}, fh, ensure_ascii=False, indent=2)
    print(f"산출 → {pref}_results.csv, {pref}_curve.csv, {pref}_tests.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
