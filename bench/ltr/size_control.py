"""`complexity`가 파일 크기 대리변수인지 검증한다.

## 왜 이걸 따로 재는가

`complexity` 단독이 전체 MRR에서 모든 그래프 baseline을 크게 이긴다. 그런데
`logical_loc` 단독도 상당히 높다. 큰 파일이 더 자주 수정된다는 것은 결함 예측
문헌의 오래된 관찰이고, Kochhar 등(ASE 2014)이 지적한 대로 **크기를 통제하지 않은
"복잡도가 예측한다"는 주장은 대부분 크기가 예측한 것**이다.

그래서 세 가지 각도로 본다. 한 가지만으로는 안 된다 — 셋이 엇갈리면 그것도 결과다.

1. **잔차 랭킹.** 인스턴스 안에서 `complexity`를 `logical_loc`에 회귀시키고 잔차로
   정렬한다. "같은 크기의 파일들 중 유난히 복잡한 것"이 gold인가를 묻는다.
   잔차 랭킹이 무너지면 complexity의 신호는 크기가 전부다.

2. **크기 매칭.** gold 파일마다 같은 인스턴스에서 `logical_loc`이 가장 가까운
   비gold 파일을 짝지어 `complexity`를 비교한다. 짝지은 비교라 크기가 설계상
   통제된다. 매칭 품질(크기 차이 분포)을 반드시 함께 본다 — 짝이 멀면 통제가 아니다.

3. **크기 구간별 분해.** 인스턴스 안에서 `logical_loc` 사분위 구간을 나누고
   구간마다 gold 비율과 complexity 차이를 본다. 효과가 특정 크기대에만 있는지 본다.

## 잔차를 어떻게 구하는가

인스턴스 안에서 **순위 변환 후** 최소제곱 직선을 뺀다. `logical_loc`은 꼬리가 매우
긴 분포(0~7839)라 원값으로 회귀하면 큰 파일 몇 개가 기울기를 다 결정한다.
순위 변환은 그 지렛대를 없앤다. 원값 회귀 결과도 같이 내서 결론이 변환에
의존하지 않는지 확인한다.
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from bench.ltr.data import Dataset
from bench.ltr.evaluate import evaluate_all, paired_bootstrap, per_instance_metrics
from bench.schema import C

RESID_COL = "complexity_resid"
RESID_RAW_COL = "complexity_resid_rawscale"


def _residual(y: np.ndarray, x: np.ndarray) -> np.ndarray:
    """y에서 x에 대한 최소제곱 직선을 뺀 잔차. x가 상수면 y의 중심화값."""
    ok = np.isfinite(y) & np.isfinite(x)
    out = np.full(len(y), np.nan)
    if ok.sum() < 3 or np.nanstd(x[ok]) == 0:
        out[ok] = y[ok] - np.nanmean(y[ok])
        return out
    slope, intercept = np.polyfit(x[ok], y[ok], 1)
    out[ok] = y[ok] - (slope * x[ok] + intercept)
    return out


def add_residual_scores(features: pd.DataFrame) -> pd.DataFrame:
    """인스턴스별 `complexity ~ logical_loc` 잔차 두 종류를 붙인다."""
    df = features.copy()
    df[RESID_COL] = np.nan
    df[RESID_RAW_COL] = np.nan
    for iid, g in df.groupby(C.instance_id, sort=False):
        y = g[C.complexity].to_numpy(dtype=float)
        x = g[C.logical_loc].to_numpy(dtype=float)
        # 순위 변환: 꼬리가 긴 logical_loc의 지렛대 제거 (모듈 docstring 참조)
        yr = pd.Series(y).rank(method="average").to_numpy()
        xr = pd.Series(x).rank(method="average").to_numpy()
        df.loc[g.index, RESID_COL] = _residual(yr, xr)
        df.loc[g.index, RESID_RAW_COL] = _residual(y, x)
    return df


def correlation_report(features: pd.DataFrame) -> pd.DataFrame:
    """인스턴스별 complexity–logical_loc 상관(Spearman)의 분포."""
    rows = []
    for iid, g in features.groupby(C.instance_id, sort=True):
        y = pd.Series(g[C.complexity].to_numpy(dtype=float))
        x = pd.Series(g[C.logical_loc].to_numpy(dtype=float))
        if y.nunique() < 2 or x.nunique() < 2:
            continue
        rows.append({C.instance_id: iid,
                     "spearman": float(y.corr(x, method="spearman")),
                     "pearson": float(y.corr(x, method="pearson"))})
    d = pd.DataFrame(rows)
    return pd.DataFrame([{
        "n_instances": len(d),
        "spearman_mean": float(d["spearman"].mean()),
        "spearman_median": float(d["spearman"].median()),
        "spearman_p10": float(d["spearman"].quantile(0.10)),
        "spearman_p90": float(d["spearman"].quantile(0.90)),
        "pearson_mean": float(d["pearson"].mean()),
    }])


def size_matched_comparison(features: pd.DataFrame,
                            max_rel_gap: float = 0.20) -> tuple[pd.DataFrame, pd.DataFrame]:
    """gold ↔ 크기 최근접 비gold 짝짓기 후 complexity 비교.

    Args:
        max_rel_gap: 짝의 `logical_loc` 상대 차이 상한. 이보다 멀면 "크기 통제"가
            아니므로 별도 표에 남기고 주 비교에서 뺀다. 짝이 몇 개나 살아남았는지가
            이 분석의 신뢰도 그 자체다.

    Returns:
        (짝 표, 요약 표). 요약은 전체 짝과 gap 조건을 통과한 짝을 나눠 낸다.
    """
    pairs = []
    for iid, g in features.groupby(C.instance_id, sort=True):
        gold = g[g[C.is_positive] == 1]
        pool = g[g[C.is_positive] == 0]
        if gold.empty or pool.empty:
            continue
        pool_loc = pool[C.logical_loc].to_numpy(dtype=float)
        used: set[int] = set()
        for _, r in gold.iterrows():
            loc = float(r[C.logical_loc])
            d = np.abs(pool_loc - loc)
            # 이미 쓴 짝은 제외한다. 재사용하면 같은 대조군 파일이 여러 gold의
            # 짝이 되어 유효 표본이 부풀어난다.
            for j in used:
                d[j] = np.inf
            j = int(np.argmin(d))
            if not np.isfinite(d[j]):
                continue
            used.add(j)
            m = pool.iloc[j]
            denom = max(loc, float(m[C.logical_loc]), 1.0)
            pairs.append({
                C.instance_id: iid,
                "gold_file": r[C.file_id], "ctrl_file": m[C.file_id],
                "gold_loc": loc, "ctrl_loc": float(m[C.logical_loc]),
                "loc_gap": float(d[j]), "rel_gap": float(d[j]) / denom,
                "gold_complexity": float(r[C.complexity]),
                "ctrl_complexity": float(m[C.complexity]),
            })
    P = pd.DataFrame(pairs)
    if P.empty:
        return P, pd.DataFrame()
    P["delta"] = P["gold_complexity"] - P["ctrl_complexity"]

    def _summ(sub: pd.DataFrame, label: str) -> dict:
        if sub.empty:
            return {"set": label, "n_pairs": 0}
        rng = np.random.default_rng(11)
        d = sub["delta"].to_numpy()
        boot = d[rng.integers(0, len(d), size=(2000, len(d)))].mean(axis=1)
        return {"set": label, "n_pairs": len(sub),
                "median_rel_gap": float(sub["rel_gap"].median()),
                "gold_complexity": float(sub["gold_complexity"].mean()),
                "ctrl_complexity": float(sub["ctrl_complexity"].mean()),
                "delta": float(d.mean()),
                "ci_lo": float(np.percentile(boot, 2.5)),
                "ci_hi": float(np.percentile(boot, 97.5)),
                "pct_gold_higher": float((sub["delta"] > 0).mean())}

    summary = pd.DataFrame([
        _summ(P, "전체 짝"),
        _summ(P[P["rel_gap"] <= max_rel_gap], f"rel_gap<={max_rel_gap}"),
    ])
    return P, summary


def size_stratified(features: pd.DataFrame, n_bins: int = 4) -> pd.DataFrame:
    """인스턴스 안에서 `logical_loc` 분위 구간을 나누고 구간별 gold 비율/complexity."""
    df = features.copy()
    df["_loc_bin"] = (df.groupby(C.instance_id)[C.logical_loc]
                        .transform(lambda s: pd.qcut(s.rank(method="first"), n_bins,
                                                     labels=False, duplicates="drop")))
    rows = []
    for b, g in df.groupby("_loc_bin", sort=True):
        gold = g[g[C.is_positive] == 1]
        nong = g[g[C.is_positive] == 0]
        rows.append({
            "loc_quartile": int(b) + 1,
            "n_files": len(g), "n_gold": len(gold),
            "gold_rate": len(gold) / len(g) if len(g) else np.nan,
            "median_loc": float(g[C.logical_loc].median()),
            "gold_complexity": float(gold[C.complexity].mean()) if len(gold) else np.nan,
            "nongold_complexity": float(nong[C.complexity].mean()) if len(nong) else np.nan,
        })
    out = pd.DataFrame(rows)
    out["delta"] = out["gold_complexity"] - out["nongold_complexity"]
    return out


def residual_ranking_report(ds: Dataset) -> tuple[pd.DataFrame, dict]:
    """complexity / logical_loc / 잔차 랭킹을 같은 지표로 비교한다.

    잔차가 `logical_loc`을 못 이기면 complexity의 신호는 크기가 전부라는 뜻이다.
    반대로 잔차가 `complexity` 원값에 근접하면 크기와 독립인 성분이 실제로 있다.
    """
    df = add_residual_scores(ds.features)
    ds2 = Dataset(features=df, gold_sets=ds.gold_sets, scanned=ds.scanned,
                  feature_columns=ds.feature_columns,
                  dropped_columns=ds.dropped_columns, seg_gold=ds.seg_gold)

    rows, per = [], {}
    for col, label in [(C.complexity, "complexity"),
                       (C.logical_loc, "logical_loc"),
                       (RESID_COL, "complexity | loc (rank resid)"),
                       (RESID_RAW_COL, "complexity | loc (raw resid)")]:
        r, p = evaluate_all(ds2, df[[C.instance_id, C.file_id, col]], col, label)
        rows.extend(r)
        per[label] = p

    tests = {}
    for seg in ("all", "non_overlap"):
        base = per["logical_loc"].get(seg)
        if base is None or base.empty:
            continue
        for label in ("complexity", "complexity | loc (rank resid)"):
            cur = per[label].get(seg)
            if cur is None or cur.empty:
                continue
            tests[f"{label} - logical_loc [{seg}]"] = paired_bootstrap(cur, base)
    return pd.DataFrame(rows), tests
