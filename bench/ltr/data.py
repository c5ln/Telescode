"""LTR 입력 데이터 로딩 — 매트릭스 조인, 구간 라벨, 피처 그룹.

여기서 하는 일은 세 가지다.

1. **피처 매트릭스 + BM25 조인.** 재스캔된 `features.csv`는 `bm25`가 비어 있다
   (0/17145). BM25 seed는 `(instance_id, file_id)`로 키가 같으므로 여기서 조인하고,
   **조인 매칭률을 강제로 검증**한다 (CONTRACT.md §3, `normalize_file_id` 주석).
   조용한 0행 조인이 이 파이프라인에서 가장 비싼 실패 모드다.

2. **gold 전체 집합 복원.** 반드시 manifest에서 가져온다. 피처 매트릭스의
   `is_positive == 1`을 모으면 스캔이 못 잡은 gold가 빠져 분모가 줄고 점수가
   부풀어난다 (`bench.metrics.baseline.gold_sets_from_manifest` 주석 참조).

3. **어휘 중첩 구간 라벨.** `bench/data/vocab_overlap.csv`(소유자 `retrieval`)를
   읽어 gold를 overlap / non_overlap으로 쪼갠다. 구간 정의는 `bench/seed/vocab.py`가
   단일 출처이고, 여기서는 **재구현하지 않고 산출물을 읽기만** 한다.

## 구간은 피처가 아니다

`vocab_overlap`은 이슈 텍스트에 의존하는 **쿼리 의존 라벨**이다. 학습 입력에
넣으면 평가 구간이 그대로 모델에 새어 들어간다. 이 모듈은 구간 라벨을 gold 집합을
쪼개는 데만 쓰고 피처 행렬에는 절대 넣지 않는다.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import pandas as pd

from bench.schema import (C, COMPLEXITY_FEATURES, DATA_DIR, GRAPH_FEATURES,
                          SEMANTIC_FEATURES)

# ablation 그룹. `bench/schema.py`의 상수를 그대로 쓴다 — 여기서 목록을 다시
# 적으면 스키마가 바뀌었을 때 조용히 어긋난다.
FEATURE_GROUPS: dict[str, list[str]] = {
    "graph": list(GRAPH_FEATURES),
    "complexity": list(COMPLEXITY_FEATURES),
    "semantic": list(SEMANTIC_FEATURES),
}

COL_OVERLAP = "vocab_overlap"   # bench/seed/vocab.py의 산출 컬럼명
SEGMENTS = ("overlap", "non_overlap")

# 조인 매칭률 하한. 이보다 낮으면 file_id 정규화가 어긋난 것이므로 즉시 실패한다.
MIN_JOIN_MATCH_RATIO = 0.99


@dataclass
class Dataset:
    """한 번의 실험이 쓰는 모든 입력."""

    features: pd.DataFrame
    gold_sets: dict[str, set[str]]          # 전체 gold (스캔 밖 포함)
    scanned: dict[str, set[str]]            # 스캔이 실제로 본 파일
    feature_columns: list[str]              # 상수/전NaN 제거 후 실제 사용 피처
    dropped_columns: list[str] = field(default_factory=list)
    seg_gold: dict[str, dict[str, set[str]]] | None = None
    notes: list[str] = field(default_factory=list)

    @property
    def instance_ids(self) -> list[str]:
        return sorted(self.features[C.instance_id].unique())

    def groups_for(self, name: str) -> list[str]:
        """이 데이터셋에서 **실제로 살아남은** 그룹 피처만."""
        return [c for c in FEATURE_GROUPS[name] if c in self.feature_columns]

    def segment_instance_count(self, segment: str) -> int:
        if not self.seg_gold:
            return 0
        return len(self.seg_gold[segment])

    def segment_gold_count(self, segment: str) -> int:
        if not self.seg_gold:
            return 0
        return sum(len(v) for v in self.seg_gold[segment].values())


def gold_sets_from_manifest(manifest: pd.DataFrame) -> dict[str, set[str]]:
    """manifest의 `gold_files` → {instance_id: 전체 gold 집합}.

    `bench.metrics.baseline`과 같은 규약이다. 그쪽 함수를 import 하지 않고 복제한
    이유는 baseline이 `status == "ok"`만 남기는데 여기서도 같은 필터가 필요하고,
    manifest를 여러 repo에서 합쳐 읽기 때문이다.
    """
    ok = manifest[manifest["status"] == "ok"]
    out = {}
    for _, r in ok.iterrows():
        raw = r["gold_files"]
        files = set(str(raw).split(";")) if isinstance(raw, str) and raw else set()
        out[r[C.instance_id]] = {f for f in files if f}
    return out


def join_bm25(features: pd.DataFrame, seed_path: Path) -> tuple[pd.DataFrame, str]:
    """BM25 seed를 `(instance_id, file_id)`로 조인한다.

    재스캔된 매트릭스의 `bm25`가 비어 있을 때만 쓴다. 이미 값이 차 있으면
    그대로 둔다 — `bench-harness`가 정식 조인 경로를 커밋하면 이 함수는 no-op이 된다.

    매칭률이 `MIN_JOIN_MATCH_RATIO` 미만이면 예외를 던진다. 조인 실패는 NaN을
    남기고, NaN은 랭킹에서 최하위로 밀려 "BM25가 쓸모없다"는 그럴듯한 오답이 된다.
    """
    filled = features[C.bm25].notna().sum()
    if filled == len(features) and len(features):
        return features, f"bm25 이미 채워짐 ({filled}/{len(features)}) — 조인 생략"

    seed = pd.read_csv(seed_path)
    merged = features.drop(columns=[C.bm25, C.bm25_rank], errors="ignore").merge(
        seed[[C.instance_id, C.file_id, C.bm25, C.bm25_rank]],
        on=[C.instance_id, C.file_id], how="left")

    if len(merged) != len(features):
        raise SystemExit(
            f"BM25 조인이 행 수를 바꿨다: {len(features)} → {len(merged)}. "
            "seed에 (instance_id, file_id) 중복이 있다")

    matched = merged[C.bm25].notna().sum()
    ratio = matched / len(merged) if len(merged) else 0.0
    if ratio < MIN_JOIN_MATCH_RATIO:
        raise SystemExit(
            f"BM25 조인 매칭률 {ratio:.4f} < {MIN_JOIN_MATCH_RATIO}. "
            f"({matched}/{len(merged)} 행). file_id 정규화가 어긋났다 — "
            "normalize_file_id를 양쪽에 같게 적용했는지 확인하라")
    return merged, f"bm25 조인 {matched}/{len(merged)} ({ratio:.4f})"


def join_rerank(features: pd.DataFrame, seed_path: Path) -> tuple[pd.DataFrame, str]:
    """리랭킹 seed를 조인한다.

    **매칭률 하한을 강제하지 않는다.** 리랭커는 인스턴스당 상위 N개만 채점하므로
    (기본 50개, 후보는 평균 191개) 대부분의 행이 NaN인 것이 **정상**이다.
    NaN은 "관련 없음"이 아니라 "측정하지 않음"이며, 0으로 채우면 측정하지 않은
    것을 관련 없다고 단정하게 된다 (`run_rerank.py` 모듈 docstring 참조).

    대신 인스턴스 커버리지를 검사한다 — seed에 있는 인스턴스가 하나도 안 붙으면
    그건 file_id 정규화가 깨진 것이다.
    """
    seed = pd.read_csv(seed_path)
    merged = features.drop(columns=[C.rerank, C.rerank_rank], errors="ignore").merge(
        seed[[C.instance_id, C.file_id, C.rerank, C.rerank_rank]],
        on=[C.instance_id, C.file_id], how="left")

    if len(merged) != len(features):
        raise SystemExit(
            f"rerank 조인이 행 수를 바꿨다: {len(features)} → {len(merged)}. "
            "seed에 (instance_id, file_id) 중복이 있다")

    seed_inst = set(seed[C.instance_id])
    hit_inst = set(merged.loc[merged[C.rerank].notna(), C.instance_id])
    missed = seed_inst - hit_inst
    if missed:
        raise SystemExit(
            f"rerank seed의 인스턴스 {len(missed)}개가 하나도 안 붙었다: "
            f"{sorted(missed)[:5]}. file_id 정규화를 확인하라")

    matched = merged[C.rerank].notna().sum()
    return merged, (f"rerank 조인 {matched}/{len(merged)} "
                    f"(행 {matched / len(merged):.3f} · 인스턴스 {len(hit_inst)}/{len(seed_inst)})")


def join_embed(features: pd.DataFrame, seed_path: Path) -> tuple[pd.DataFrame, str]:
    """임베딩 seed를 `(instance_id, file_id)`로 조인한다. `join_bm25`와 대칭이다.

    `bench/features/extract.py`가 `embed`/`embed_rank`를 NaN으로 내보내므로
    (스키마는 있고 값은 없다), 여기서 채운다.

    **BM25와 달리 매칭률 하한을 강제하지 않는다.** 임베딩은 선택적 신호라
    seed 파일이 아예 없을 수 있고, 그때는 NaN인 채로 두는 게 맞다. 다만 seed를
    **주었는데** 안 붙는 것은 조용한 실패이므로, 그 경우에는 BM25와 같은 기준으로 막는다.
    """
    seed = pd.read_csv(seed_path)
    merged = features.drop(columns=[C.embed, C.embed_rank], errors="ignore").merge(
        seed[[C.instance_id, C.file_id, C.embed, C.embed_rank]],
        on=[C.instance_id, C.file_id], how="left")

    if len(merged) != len(features):
        raise SystemExit(
            f"embed 조인이 행 수를 바꿨다: {len(features)} → {len(merged)}. "
            "seed에 (instance_id, file_id) 중복이 있다")

    matched = merged[C.embed].notna().sum()
    ratio = matched / len(merged) if len(merged) else 0.0
    if ratio < MIN_JOIN_MATCH_RATIO:
        raise SystemExit(
            f"embed 조인 매칭률 {ratio:.4f} < {MIN_JOIN_MATCH_RATIO}. "
            f"({matched}/{len(merged)} 행). file_id 정규화가 어긋났다 — "
            "normalize_file_id를 양쪽에 같게 적용했는지 확인하라")
    return merged, f"embed 조인 {matched}/{len(merged)} ({ratio:.4f})"


def load_segments(features: pd.DataFrame, overlap_path: Path,
                  condition: str = "full") -> tuple[dict, list[str]]:
    """`vocab_overlap.csv` → {segment: {instance_id: gold 집합}}.

    `bench/seed/run_vocab.py`의 `segment_gold`와 같은 규칙이다: gold 행만 남기고
    `vocab_overlap` 플래그로 쪼갠다. 스캔에 없는 gold는 행 자체가 없어 어느 구간에도
    들어가지 않으므로 unclassified로 따로 센다.
    """
    notes = []
    if not overlap_path.exists():
        return {s: {} for s in SEGMENTS}, [f"{overlap_path} 없음 — 구간 분석 생략"]

    ov = pd.read_csv(overlap_path)
    ov = ov[ov["condition"] == condition]
    covered = set(ov[C.instance_id])
    have = set(features[C.instance_id])
    if not covered & have:
        return {s: {} for s in SEGMENTS}, [
            f"vocab_overlap({condition})가 매트릭스 인스턴스를 하나도 안 덮는다 — 구간 분석 생략"]

    missing = have - covered
    if missing:
        notes.append(
            f"구간 라벨 없는 인스턴스 {len(missing)}개는 구간 분석에서 제외 "
            f"(전체 {len(have)}개 중). vocab_overlap은 xarray만 덮는다")

    gpos = ov[ov[C.is_positive] == 1]
    seg = {
        "overlap": gpos[gpos[COL_OVERLAP] == 1].groupby(C.instance_id)[C.file_id]
                       .apply(set).to_dict(),
        "non_overlap": gpos[gpos[COL_OVERLAP] == 0].groupby(C.instance_id)[C.file_id]
                           .apply(set).to_dict(),
    }
    # 라벨은 있는데 매트릭스에 없는 인스턴스는 버린다 (구간 gold 수를 부풀리지 않게).
    seg = {k: {i: v for i, v in d.items() if i in have} for k, d in seg.items()}
    return seg, notes


def usable_features(df: pd.DataFrame, columns: list[str]) -> tuple[list[str], list[str]]:
    """전부 NaN이거나 전 구간 상수인 컬럼을 뺀다.

    `ppr`은 Phase 3이 안 끝나 전부 NaN이고, 상수 컬럼은 트리에 아무 정보도 주지
    않으면서 ablation 표에 "그룹이 있었다"는 착시를 만든다. 뺀 목록을 반드시 보고한다.
    """
    keep, dropped = [], []
    for c in columns:
        s = df[c]
        if s.isna().all():
            dropped.append(f"{c}(전부 NaN)")
        elif s.nunique(dropna=True) <= 1:
            dropped.append(f"{c}(상수)")
        else:
            keep.append(c)
    return keep, dropped


def load_dataset(features_path: Path, manifest_path: Path,
                 *, bm25_seed_path: Path | None = None,
                 embed_seed_path: Path | None = None,
                 rerank_seed_path: Path | None = None,
                 overlap_path: Path | None = None,
                 overlap_condition: str = "full",
                 exclude_generated: bool = False) -> Dataset:
    """실험 입력 일괄 로딩 + 무결성 검증."""
    notes: list[str] = []
    features = pd.read_csv(features_path)
    manifest = pd.read_csv(manifest_path)

    if bm25_seed_path is not None and bm25_seed_path.exists():
        features, msg = join_bm25(features, bm25_seed_path)
        notes.append(msg)
    elif features[C.bm25].isna().all():
        notes.append("bm25 전부 비어 있고 seed 경로도 없다 — semantic 그룹 없이 진행")

    if embed_seed_path is not None and embed_seed_path.exists():
        features, msg = join_embed(features, embed_seed_path)
        notes.append(msg)
    else:
        notes.append("embed seed 없음 — 임베딩 없이 진행")

    if rerank_seed_path is not None and rerank_seed_path.exists():
        features, msg = join_rerank(features, rerank_seed_path)
        notes.append(msg)
    else:
        notes.append("rerank seed 없음 — 리랭킹 없이 진행")

    if exclude_generated:
        before = len(features)
        features = features[features[C.is_generated] == 0].copy()
        notes.append(f"is_generated 후보 제외: {before} → {len(features)}행")

    gold_sets = gold_sets_from_manifest(manifest)
    missing = set(features[C.instance_id]) - set(gold_sets)
    if missing:
        raise SystemExit(f"manifest에 없는 인스턴스 {len(missing)}개: {sorted(missing)[:5]}")

    scanned = {k: set(v) for k, v in
               features.groupby(C.instance_id)[C.file_id].apply(set).items()}

    cols = [c for c in GRAPH_FEATURES + COMPLEXITY_FEATURES + SEMANTIC_FEATURES
            if c in features.columns]
    keep, dropped = usable_features(features, cols)
    if dropped:
        notes.append("사용 불가 피처 제외: " + ", ".join(dropped))

    seg_gold, seg_notes = ({s: {} for s in SEGMENTS}, [])
    if overlap_path is not None:
        seg_gold, seg_notes = load_segments(features, overlap_path, overlap_condition)
    notes.extend(seg_notes)

    n_gold = sum(len(v) for v in gold_sets.values())
    n_in_scan = int(features[C.is_positive].sum())
    notes.append(
        f"gold {n_gold}개 중 스캔에 있는 것 {n_in_scan}개 "
        f"(차이 {n_gold - n_in_scan}개가 Recall 상한을 깎는다)")

    return Dataset(features=features, gold_sets=gold_sets, scanned=scanned,
                   feature_columns=keep, dropped_columns=dropped,
                   seg_gold=seg_gold, notes=notes)


def per_instance_minmax(df: pd.DataFrame, columns: list[str]) -> pd.DataFrame:
    """인스턴스 안에서의 min-max 정규화.

    랭킹은 **인스턴스 안에서** 매겨지는데 `bc`(0~4778)나 `logical_loc`(0~7839)은
    인스턴스마다 스케일이 다르다. 정규화하지 않으면 트리가 "이 인스턴스는 큰
    저장소인가"를 학습해 버린다. 그건 랭킹과 무관한 그룹 수준 신호다.

    전 구간 동일값인 컬럼은 0을 준다 (`bench.metrics.baseline.minmax`와 같은 규약).
    NaN은 그대로 둔다 — LightGBM이 결측을 직접 처리한다.
    """
    out = df[columns].astype(float).copy()
    g = df.groupby(C.instance_id)[columns]
    lo, hi = g.transform("min"), g.transform("max")
    span = (hi - lo).to_numpy()
    vals = (out.to_numpy() - lo.to_numpy())
    with np.errstate(invalid="ignore", divide="ignore"):
        norm = np.where(span > 0, vals / np.where(span > 0, span, 1.0), 0.0)
    # span이 NaN인(그 인스턴스에서 전부 결측인) 칸까지 0으로 덮이면 결측이 조용히
    # "최솟값"이 된다. 원본이 NaN인 자리는 NaN으로 되돌린다.
    norm = np.where(np.isnan(out.to_numpy()), np.nan, norm)
    return pd.DataFrame(norm, columns=columns, index=df.index)
