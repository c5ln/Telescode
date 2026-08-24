"""Reciprocal Rank Fusion — 학습 없는 hybrid 결합.

## 왜 LTR 옆에 이걸 두는가

전체 구간에서는 어휘 검색이, 어휘 비중첩 구간에서는 구조 신호가 이긴다. 두 신호가
서로 다른 구간에서 작동하므로 결합에 근거가 있다. 그런데 **전역으로 학습한 LTR은
이 구조를 이용하지 못한다.** 학습 손실이 전체 인스턴스 평균이라, 인스턴스 대다수가
속한 어휘 중첩 구간에 맞춰 최적화되고 비중첩 구간은 부수적으로 딸려온다.

RRF는 학습이 없으므로 그 편향이 없다. 점수 스케일도 안 맞춰도 된다 — 순위만 쓴다.
LTR이 결합의 이득을 못 낸 것인지, 결합 자체에 이득이 없는 것인지를 가른다.

    RRF(f) = Σ_r  1 / (k + rank_r(f))

`k`는 상위 순위의 지배력을 눌러 주는 상수다. Cormack 등(SIGIR 2009)의 `k=60`을
그대로 쓴다. 이 값을 우리 test 지표를 보고 고르면 무결성 규칙 §4 위반이므로
**문헌 기본값에서 움직이지 않는다.**

## 순위는 비관적 규칙으로 매긴다

`bench.metrics.ranking.apply_pessimistic_tiebreak`과 같은 이유다. `pagerank`처럼
대다수 파일이 바닥값으로 동점인 컬럼에서 낙관적 순위를 주면 융합 점수가 부풀어난다.
여기서는 gold를 모르는 상태로 순위를 매겨야 하므로(랭커는 정답을 못 본다) 동점은
`file_id` 사전순으로 깨고, 최종 지표 계산은 언제나처럼 `evaluate_instance`가
비관적으로 다시 처리한다.
"""

from __future__ import annotations

import pandas as pd

from bench.schema import C

RRF_K = 60


def _ranks(g: pd.DataFrame, col: str, higher_is_better: bool) -> pd.Series:
    """인스턴스 안에서의 1-기반 순위. 동점은 평균 순위로 준다.

    평균 순위는 동점 그룹의 모든 파일에 같은 융합 기여를 준다 — 정답을 모르는
    상태에서 동점을 임의로 깨는 것보다 정직하다.
    """
    s = g[col].astype(float)
    return s.rank(ascending=not higher_is_better, method="average")


def rrf(features: pd.DataFrame, rankers: list[tuple[str, bool]],
        *, k: int = RRF_K, out_col: str = "rrf") -> pd.DataFrame:
    """`rankers`(컬럼, 클수록 좋은가) 목록을 RRF로 융합한다.

    Returns:
        `instance_id`, `file_id`, `out_col` 세 컬럼. 융합 점수는 클수록 좋다.
    """
    parts = []
    for iid, g in features.groupby(C.instance_id, sort=False):
        score = 0.0
        for col, hib in rankers:
            score = score + 1.0 / (k + _ranks(g, col, hib))
        parts.append(pd.DataFrame({C.instance_id: g[C.instance_id].to_numpy(),
                                   C.file_id: g[C.file_id].to_numpy(),
                                   out_col: score.to_numpy()}))
    return pd.concat(parts, ignore_index=True)


def with_external(features: pd.DataFrame, external: pd.DataFrame,
                  ext_col: str) -> pd.DataFrame:
    """LTR OOF 점수처럼 매트릭스 밖에 있는 점수 컬럼을 붙인다.

    조인 후 행 수와 결측을 검증한다 — 말없이 NaN이 남으면 그 파일은 순위 최하위로
    밀려 융합 결과가 조용히 틀어진다.
    """
    merged = features.merge(external[[C.instance_id, C.file_id, ext_col]],
                            on=[C.instance_id, C.file_id], how="left")
    if len(merged) != len(features):
        raise SystemExit(f"외부 점수 조인이 행 수를 바꿨다: {len(features)} → {len(merged)}")
    n_missing = int(merged[ext_col].isna().sum())
    if n_missing:
        raise SystemExit(f"외부 점수 조인에 결측 {n_missing}행 — 키가 어긋났다")
    return merged
