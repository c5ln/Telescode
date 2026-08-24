"""점수 컬럼 → 전체/구간 지표, 그리고 짝지은 부트스트랩 신뢰구간.

지표 계산은 전부 `bench.metrics.ranking.evaluate_instance`를 통과한다 (계약 §2).
여기 있는 것은 지표 구현이 아니라 **구간 분리와 집계**다.

## 구간 분리는 gold를 쪼개서 한다

후보 랭킹은 전체를 유지하고 "이 구간의 gold를 얼마나 위로 올렸는가"를 본다.
후보를 쪼개면 순위 자체가 바뀌어 다른 것을 재게 된다. `bench/seed/run_vocab.py`와
같은 규약이다.

## 왜 부트스트랩을 같이 내는가

xarray non_overlap 구간은 인스턴스 16개다. 이 크기에서 MRR 차이 0.05는 눈으로는
커 보여도 표본 노이즈와 구분되지 않는다. 점추정만 적어 두면 읽는 사람이 그걸
결론으로 만든다. **인스턴스 단위 짝지은 부트스트랩**으로 차이의 구간을 함께 낸다 —
같은 인스턴스에서 두 랭커를 비교하므로 인스턴스 난이도 차이가 상쇄된다.
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from bench.ltr.data import SEGMENTS, Dataset
from bench.metrics.ranking import evaluate_instance
from bench.schema import C

KS = (1, 3, 5, 10, 20, 50)
REPORT_KS = (1, 5, 10, 20)
N_BOOTSTRAP = 2000
BOOT_SEED = 7


def per_instance_metrics(ds: Dataset, scores: pd.DataFrame, score_col: str,
                         *, higher_is_better: bool = True,
                         segment: str | None = None) -> pd.DataFrame:
    """인스턴스별 지표 표. `segment`가 주어지면 그 구간의 gold만 분모로 쓴다.

    구간 gold가 없는 인스턴스는 **행 자체를 만들지 않는다.** NaN으로 남기면
    `nanmean`이 알아서 빼 주지만, `n_instances`가 부풀어 "16개로 잰 숫자"가
    "110개로 잰 숫자"처럼 보인다. 이 실험에서 그건 치명적이다.
    """
    sign = 1.0 if higher_is_better else -1.0
    rows = []
    for iid, g in scores.groupby(C.instance_id, sort=True):
        if segment is None:
            gold = ds.gold_sets.get(iid, set())
        else:
            gold = (ds.seg_gold or {}).get(segment, {}).get(iid, set())
        if not gold:
            continue
        vals = sign * g[score_col].to_numpy(dtype=float)
        rec = evaluate_instance(g[C.file_id].tolist(), vals, gold,
                                scanned_file_ids=ds.scanned[iid], ks=KS)
        rec[C.instance_id] = iid
        rows.append(rec)
    return pd.DataFrame(rows)


def summarize(m: pd.DataFrame, label: str, segment: str = "all") -> dict:
    """인스턴스 평균. `n_instances`와 `n_gold`를 **항상** 같이 낸다 (팀 규칙 3)."""
    if m.empty:
        return {"scorer": label, "segment": segment, "n_instances": 0, "n_gold": 0}
    row = {"scorer": label, "segment": segment,
           "n_instances": len(m), "n_gold": int(m["n_gold"].sum()),
           "ceiling": float(np.nanmean(m["reachable_recall_ceiling"])),
           "MRR": float(np.nanmean(m["mrr"]))}
    for k in REPORT_KS:
        row[f"R@{k}"] = float(np.nanmean(m[f"recall@{k}"]))
    row["NDCG@10"] = float(np.nanmean(m["ndcg@10"]))
    return row


def evaluate_all(ds: Dataset, scores: pd.DataFrame, score_col: str, label: str,
                 *, higher_is_better: bool = True
                 ) -> tuple[list[dict], dict[str, pd.DataFrame]]:
    """전체 + 각 구간의 요약 행과, 부트스트랩용 인스턴스별 표를 함께 돌려준다."""
    out, per = [], {}
    m = per_instance_metrics(ds, scores, score_col, higher_is_better=higher_is_better)
    out.append(summarize(m, label, "all"))
    per["all"] = m
    for seg in SEGMENTS:
        if not (ds.seg_gold or {}).get(seg):
            continue
        ms = per_instance_metrics(ds, scores, score_col,
                                  higher_is_better=higher_is_better, segment=seg)
        out.append(summarize(ms, label, seg))
        per[seg] = ms
    return out, per


def paired_bootstrap(a: pd.DataFrame, b: pd.DataFrame, metric: str = "mrr",
                     n: int = N_BOOTSTRAP, seed: int = BOOT_SEED) -> dict:
    """`a - b`의 인스턴스 단위 짝지은 부트스트랩.

    두 표를 `instance_id`로 **내부 조인**한다. 한쪽에만 있는 인스턴스를 섞으면
    짝지음이 깨져 인스턴스 난이도 차이가 그대로 분산에 들어간다.

    Returns:
        diff(점추정), ci_lo/ci_hi(2.5/97.5 백분위), p_two_sided(부호 검정 근사),
        n_paired. `n_paired`가 20 미만이면 `underpowered=True`를 붙인다 —
        팀 규칙 3의 노이즈 기준이다.
    """
    if a.empty or b.empty:
        return {"n_paired": 0, "underpowered": True}
    j = a[[C.instance_id, metric]].merge(
        b[[C.instance_id, metric]], on=C.instance_id, suffixes=("_a", "_b"))
    d = (j[f"{metric}_a"] - j[f"{metric}_b"]).to_numpy(dtype=float)
    d = d[~np.isnan(d)]
    if len(d) == 0:
        return {"n_paired": 0, "underpowered": True}

    rng = np.random.default_rng(seed)
    idx = rng.integers(0, len(d), size=(n, len(d)))
    boot = d[idx].mean(axis=1)
    # 부트스트랩 분포가 0의 어느 쪽에 몰려 있는지. 정규성 가정 없는 양측 근사다.
    p = 2.0 * min((boot <= 0).mean(), (boot >= 0).mean())
    return {"diff": float(d.mean()),
            "ci_lo": float(np.percentile(boot, 2.5)),
            "ci_hi": float(np.percentile(boot, 97.5)),
            "p_two_sided": float(min(1.0, p)),
            "n_paired": int(len(d)),
            "underpowered": bool(len(d) < 20)}


def fmt(df: pd.DataFrame) -> str:
    return df.to_string(index=False, float_format=lambda v: f"{v:.4f}")
