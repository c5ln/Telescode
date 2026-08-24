"""LambdaRank 학습 + 누수 없는 out-of-fold 예측.

## 왜 held-out 단일 분할이 아니라 OOF CV인가

`bench/data/splits.csv`는 인스턴스의 25%를 test로 뗀다. xarray 110개 기준 test는
28개다. 그 안에서 non_overlap 구간은 4개 남짓이 되는데, 팀 규칙 3(구간 크기를 항상
보고하고 n<20 부분집합 수치는 신뢰하지 않는다)에 따르면 그 숫자는 보고할 수 없다.

그래서 **주 평가는 outer 5-fold grouped CV의 out-of-fold 예측**이다. 모든 인스턴스가
정확히 한 번 held-out이 되므로 평가 n이 전체 인스턴스 수와 같아지면서도 각 예측은
그 인스턴스를 못 본 모델이 만든다. pin된 `splits.csv` 분할은 **부차 확인**으로 함께
낸다 — 두 숫자가 크게 어긋나면 CV 쪽을 의심해야 한다.

## 하이퍼파라미터

무결성 규칙 §4: test를 보고 고르면 그 숫자는 폐기다. outer fold마다 **그 fold의
train 안에서만** inner grouped CV를 돌려 고른다 (nested CV). outer test는 선택에
전혀 관여하지 않는다.

## is_positive == 0 문제 (계약 §1의 경고)

계약은 0을 negative로 쓰지 말라고 한다. LambdaRank는 형식상 0을 최하위 등급으로
넣는 수밖에 없으므로 **이 제약을 완전히는 만족시키지 못한다.** 그대로 적는다:

  - 라벨 0은 "안 읽어도 되는 파일"이 아니라 "이 패치가 안 고친 파일"이다.
    gold ⊂ 읽어야 할 파일이므로, 구조적으로 중심적이지만 수정되지 않은 파일이
    계속 오답으로 벌점을 받는다. 즉 **그래프 피처에 불리한 방향의 편향**이다.
  - 따라서 "LTR이 그래프 피처에 낮은 가중치를 줬다"를 "구조 신호가 쓸모없다"로
    읽으면 안 된다. 그 질문에 답하는 것은 gain 값이 아니라 **구간별 ablation**이다.
  - 편향 방향이 한쪽(그래프에 불리)이라 결과가 긍정적으로 나오면 그건 하한이다.

완화 장치로 `label_gain`을 손대지 않고 기본값을 쓴다. unlabeled를 임의 가중치로
끌어올리면 그 가중치가 결론을 만든다.
"""

from __future__ import annotations

import warnings
from dataclasses import dataclass

import numpy as np
import pandas as pd

from bench.ltr.data import Dataset, per_instance_minmax
from bench.schema import C

try:
    import lightgbm as lgb
    HAVE_LGBM = True
except ImportError:                                     # pragma: no cover
    HAVE_LGBM = False

N_OUTER_FOLDS = 5
N_INNER_FOLDS = 4
SEED = 42

# 작은 데이터(인스턴스 82개 × ~150파일)에 맞춘 좁은 그리드.
# 넓게 훑으면 inner CV가 test 없이도 과적합되고, 무엇보다 실행 시간이 결론을 못 바꾼다.
PARAM_GRID = [
    {"num_leaves": 7,  "min_child_samples": 30, "learning_rate": 0.05, "n_estimators": 200},
    {"num_leaves": 15, "min_child_samples": 20, "learning_rate": 0.05, "n_estimators": 300},
    {"num_leaves": 31, "min_child_samples": 20, "learning_rate": 0.10, "n_estimators": 200},
    {"num_leaves": 7,  "min_child_samples": 50, "learning_rate": 0.10, "n_estimators": 100},
]

BASE_PARAMS = dict(
    objective="lambdarank",
    # eval_at은 넣지 않는다. eval set을 안 쓰므로 학습에 아무 영향이 없으면서
    # LGBMRanker의 동명 인자와 충돌해 fit마다 UserWarning을 찍는다.
    metric="ndcg",
    boosting_type="gbdt",
    subsample=0.9,
    subsample_freq=1,
    colsample_bytree=0.9,
    reg_lambda=1.0,
    random_state=SEED,
    n_jobs=4,
    verbose=-1,
)


@dataclass
class FoldAssignment:
    """instance_id → outer fold 번호. 해시 기반이라 재현된다."""

    mapping: dict[str, int]
    n_folds: int

    def fold_of(self, iid: str) -> int:
        return self.mapping[iid]


def make_folds(instance_ids, n_folds: int = N_OUTER_FOLDS,
               seed: int = SEED) -> FoldAssignment:
    """**instance_id 단위** fold 배정 (무결성 규칙 §1).

    `bench.metrics.baseline.make_splits`의 해시 방식과 달리 여기서는 fold 크기를
    균등하게 맞춘다 — 해시 나머지는 fold마다 인스턴스 수가 들쭉날쭉해서, 가장 작은
    fold의 구간 gold가 0이 되면 그 fold의 OOF가 통째로 빠진다.
    """
    ids = sorted(set(instance_ids))
    rng = np.random.default_rng(seed)
    order = rng.permutation(len(ids))
    mapping = {ids[j]: int(i % n_folds) for i, j in enumerate(order)}
    return FoldAssignment(mapping=mapping, n_folds=n_folds)


def _prepare(ds: Dataset, columns: list[str], normalize: bool) -> pd.DataFrame:
    """모델 입력 행렬. 정규화 근거는 `data.per_instance_minmax` 참조."""
    if normalize:
        return per_instance_minmax(ds.features, columns)
    return ds.features[columns].astype(float)


def _group_sizes(df: pd.DataFrame, idx: np.ndarray) -> np.ndarray:
    """LightGBM `group` 인자. 행이 instance_id로 **연속 정렬**돼 있어야 한다."""
    return df.iloc[idx].groupby(C.instance_id, sort=False).size().to_numpy()


def _fit(X: pd.DataFrame, y: np.ndarray, groups: np.ndarray, params: dict):
    model = lgb.LGBMRanker(**{**BASE_PARAMS, **params})
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        model.fit(X, y, group=groups)
    return model


def _ndcg_at_10(scores: np.ndarray, labels: np.ndarray, group_sizes: np.ndarray) -> float:
    """inner CV 선택 전용 NDCG@10.

    `bench.metrics.ranking`을 쓰지 않는 유일한 자리다. 그쪽은 file_id와 gold 집합을
    받는데 여기서는 **하이퍼파라미터 선택 신호**만 필요하고 전체 gold(스캔 밖 포함)
    분모도 필요 없다. 보고되는 숫자는 전부 `bench.metrics.ranking`을 거친다.
    """
    out, pos = [], 0
    for n in group_sizes:
        s, l = scores[pos:pos + n], labels[pos:pos + n]
        pos += n
        if l.sum() == 0:
            continue
        order = np.argsort(-s, kind="stable")
        gains = l[order][:10]
        disc = 1.0 / np.log2(np.arange(2, len(gains) + 2))
        dcg = float((gains * disc).sum())
        ideal = min(int(l.sum()), 10)
        idcg = float((1.0 / np.log2(np.arange(2, ideal + 2))).sum())
        out.append(dcg / idcg if idcg else np.nan)
    return float(np.nanmean(out)) if out else float("nan")


def _select_params(df: pd.DataFrame, X: pd.DataFrame, y: np.ndarray,
                   train_ids: list[str], seed: int) -> tuple[dict, float]:
    """outer train 안에서만 도는 inner grouped CV (무결성 규칙 §4)."""
    inner = make_folds(train_ids, N_INNER_FOLDS, seed=seed + 1)
    best, best_score = PARAM_GRID[0], -np.inf
    for params in PARAM_GRID:
        scores = []
        for f in range(N_INNER_FOLDS):
            va_ids = [i for i in train_ids if inner.fold_of(i) == f]
            tr_ids = [i for i in train_ids if inner.fold_of(i) != f]
            tr = np.flatnonzero(df[C.instance_id].isin(tr_ids).to_numpy())
            va = np.flatnonzero(df[C.instance_id].isin(va_ids).to_numpy())
            if not len(tr) or not len(va) or y[tr].sum() == 0:
                continue
            m = _fit(X.iloc[tr], y[tr], _group_sizes(df, tr), params)
            scores.append(_ndcg_at_10(m.predict(X.iloc[va]), y[va],
                                      _group_sizes(df, va)))
        s = float(np.nanmean(scores)) if scores else float("nan")
        if np.isfinite(s) and s > best_score:
            best, best_score = params, s
    return best, best_score


@dataclass
class OOFResult:
    scores: pd.DataFrame            # instance_id, file_id, ltr_score
    chosen_params: list[dict]       # outer fold별 선택된 하이퍼파라미터
    inner_scores: list[float]
    gain: pd.DataFrame              # 피처별 평균 gain (fold 평균)
    n_folds: int
    columns: list[str]


def oof_predict(ds: Dataset, columns: list[str], *, normalize: bool = True,
                folds: FoldAssignment | None = None,
                tune: bool = True, seed: int = SEED) -> OOFResult:
    """nested CV로 모든 인스턴스의 out-of-fold 점수를 만든다.

    Args:
        columns: 이번 실행에서 쓸 피처. ablation은 여기서 그룹을 빼는 것으로 한다.
        normalize: 인스턴스 내 min-max 정규화 (`data.per_instance_minmax`).
        folds: 실행 간 fold를 고정하려면 넘긴다. ablation 비교는 **반드시 같은
            fold**여야 한다 — fold가 다르면 차이가 모델 탓인지 분할 탓인지 모른다.
        tune: False면 `PARAM_GRID[0]`을 그대로 쓴다 (빠른 회귀 확인용).
    """
    if not HAVE_LGBM:
        raise SystemExit("lightgbm이 없다: bench/.venv/bin/pip install lightgbm")
    if not columns:
        raise ValueError("피처가 비었다 — 그룹을 전부 빼면 학습할 것이 없다")

    df = ds.features.sort_values([C.instance_id, C.file_id]).reset_index(drop=True)
    ds_sorted = Dataset(features=df, gold_sets=ds.gold_sets, scanned=ds.scanned,
                        feature_columns=ds.feature_columns,
                        dropped_columns=ds.dropped_columns, seg_gold=ds.seg_gold)
    X = _prepare(ds_sorted, columns, normalize)
    y = df[C.is_positive].to_numpy(dtype=int)

    ids = sorted(df[C.instance_id].unique())
    folds = folds or make_folds(ids, N_OUTER_FOLDS, seed)

    oof = np.full(len(df), np.nan)
    chosen, inner_scores, gains = [], [], []
    for f in range(folds.n_folds):
        te_ids = [i for i in ids if folds.fold_of(i) == f]
        tr_ids = [i for i in ids if folds.fold_of(i) != f]
        tr = np.flatnonzero(df[C.instance_id].isin(tr_ids).to_numpy())
        te = np.flatnonzero(df[C.instance_id].isin(te_ids).to_numpy())

        if tune:
            params, s = _select_params(df, X, y, tr_ids, seed + 10 * f)
        else:
            params, s = PARAM_GRID[0], float("nan")
        chosen.append(params)
        inner_scores.append(s)

        m = _fit(X.iloc[tr], y[tr], _group_sizes(df, tr), params)
        oof[te] = m.predict(X.iloc[te])
        gains.append(pd.Series(m.booster_.feature_importance("gain"), index=columns))

    if np.isnan(oof).any():
        raise RuntimeError(f"OOF 예측에 NaN {int(np.isnan(oof).sum())}행 — fold 배정 누락")

    gain = pd.concat(gains, axis=1).mean(axis=1).sort_values(ascending=False)
    gain = (gain / gain.sum() if gain.sum() else gain).rename("gain_share").to_frame()

    return OOFResult(
        scores=pd.DataFrame({C.instance_id: df[C.instance_id],
                             C.file_id: df[C.file_id],
                             "ltr_score": oof}),
        chosen_params=chosen, inner_scores=inner_scores,
        gain=gain, n_folds=folds.n_folds, columns=list(columns))


def holdout_predict(ds: Dataset, columns: list[str], splits: pd.DataFrame,
                    *, normalize: bool = True, tune: bool = True,
                    seed: int = SEED) -> pd.DataFrame:
    """pin된 `splits.csv` 분할로 학습 → test 인스턴스 점수. 부차 확인용.

    하이퍼파라미터는 train의 `cv_fold`로만 고른다. test는 선택에 안 쓴다.
    """
    if not HAVE_LGBM:
        raise SystemExit("lightgbm이 없다: bench/.venv/bin/pip install lightgbm")

    df = ds.features.sort_values([C.instance_id, C.file_id]).reset_index(drop=True)
    ds_sorted = Dataset(features=df, gold_sets=ds.gold_sets, scanned=ds.scanned,
                        feature_columns=ds.feature_columns,
                        dropped_columns=ds.dropped_columns, seg_gold=ds.seg_gold)
    X = _prepare(ds_sorted, columns, normalize)
    y = df[C.is_positive].to_numpy(dtype=int)

    sp = splits.set_index(C.instance_id)["split"].to_dict()
    tr_ids = [i for i in sorted(df[C.instance_id].unique()) if sp.get(i) == "train"]
    te_ids = [i for i in sorted(df[C.instance_id].unique()) if sp.get(i) == "test"]
    tr = np.flatnonzero(df[C.instance_id].isin(tr_ids).to_numpy())
    te = np.flatnonzero(df[C.instance_id].isin(te_ids).to_numpy())

    params = _select_params(df, X, y, tr_ids, seed)[0] if tune else PARAM_GRID[0]
    m = _fit(X.iloc[tr], y[tr], _group_sizes(df, tr), params)
    return pd.DataFrame({C.instance_id: df[C.instance_id].iloc[te].to_numpy(),
                         C.file_id: df[C.file_id].iloc[te].to_numpy(),
                         "ltr_score": m.predict(X.iloc[te])})
