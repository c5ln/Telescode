"""seed ↔ 피처 매트릭스 조인 검증.

CONTRACT.md §1: `file_id` 정규화가 어긋나면 조인이 **조용히 0행**이 된다.
그래서 조인 후 매칭 행 수를 세고, 임계 미만이면 실패로 처리한다.
"""

from __future__ import annotations

from dataclasses import dataclass

import pandas as pd

from bench.schema import C, KEY_COLUMNS, normalize_file_id


@dataclass
class JoinReport:
    n_features: int
    n_seed: int
    n_matched: int
    seed_only: int          # seed에는 있는데 매트릭스에 없는 (file_id) 수
    features_only: int      # 매트릭스에는 있는데 seed에 없는 수
    coverage: float         # 매트릭스 행 중 bm25가 채워진 비율
    instances_matched: int
    instances_zero_match: list[str]

    def ok(self, min_coverage: float = 0.80) -> bool:
        return self.n_matched > 0 and not self.instances_zero_match \
            and self.coverage >= min_coverage


def normalize(df: pd.DataFrame) -> pd.DataFrame:
    df = df.copy()
    df[C.file_id] = df[C.file_id].astype(str).map(normalize_file_id)
    return df


def join_seed(features: pd.DataFrame, seed: pd.DataFrame) -> tuple[pd.DataFrame, JoinReport]:
    """매트릭스에 bm25/bm25_rank를 left-join 하고 매칭 현황을 보고한다."""
    f = normalize(features)
    s = normalize(seed)[KEY_COLUMNS + [C.bm25, C.bm25_rank]]

    merged = f.drop(columns=[c for c in (C.bm25, C.bm25_rank) if c in f.columns]) \
              .merge(s, on=KEY_COLUMNS, how="left")

    fkeys = set(map(tuple, f[KEY_COLUMNS].itertuples(index=False, name=None)))
    skeys = set(map(tuple, s[KEY_COLUMNS].itertuples(index=False, name=None)))
    matched = fkeys & skeys

    per_inst = merged.groupby(C.instance_id)[C.bm25].apply(lambda x: x.notna().sum())
    zero = sorted(per_inst[per_inst == 0].index.tolist())

    rep = JoinReport(
        n_features=len(f), n_seed=len(s), n_matched=len(matched),
        seed_only=len(skeys - fkeys), features_only=len(fkeys - skeys),
        coverage=float(merged[C.bm25].notna().mean()) if len(merged) else 0.0,
        instances_matched=int((per_inst > 0).sum()),
        instances_zero_match=zero,
    )
    return merged, rep
