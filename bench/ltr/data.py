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

from bench.features.code_lexical import EXTRA_COLUMNS, EXTRA_GROUPS
from bench.schema import (C, COMPLEXITY_FEATURES, DATA_DIR, GRAPH_FEATURES,
                          KEY_COLUMNS, SEMANTIC_FEATURES)
from ltr.preprocessing import per_query_minmax

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
        cols = FEATURE_GROUPS[name] if name in FEATURE_GROUPS else EXTRA_GROUPS[name]
        return [c for c in cols if c in self.feature_columns]

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


def join_extra(features: pd.DataFrame, path: Path) -> tuple[pd.DataFrame, str]:
    """코드 어휘 피처(`bench.features.code_lexical`)를 조인한다.

    BM25와 같은 기준으로 매칭률을 강제한다. 네 컬럼 모두 base_commit blob과 간선에서
    나오므로 매트릭스의 모든 행이 값을 가져야 정상이다.
    """
    extra = pd.read_csv(path)
    cols = [c for c in EXTRA_COLUMNS if c in extra.columns]
    if not cols:
        raise SystemExit(f"{path}에 코드 어휘 피처 컬럼이 없다: {EXTRA_COLUMNS}")
    merged = features.drop(columns=cols, errors="ignore").merge(
        extra[KEY_COLUMNS + cols], on=KEY_COLUMNS, how="left")

    if len(merged) != len(features):
        raise SystemExit(
            f"extra 조인이 행 수를 바꿨다: {len(features)} → {len(merged)}. "
            "(instance_id, file_id) 중복이 있다")

    matched = int(merged[cols].notna().all(axis=1).sum())
    ratio = matched / len(merged) if len(merged) else 0.0
    if ratio < MIN_JOIN_MATCH_RATIO:
        raise SystemExit(
            f"extra 조인 매칭률 {ratio:.4f} < {MIN_JOIN_MATCH_RATIO} "
            f"({matched}/{len(merged)} 행)")
    return merged, f"extra 조인 {matched}/{len(merged)} ({ratio:.4f}): {', '.join(cols)}"


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


def restrict_candidates(ds: Dataset, top_k: int | None = None, *,
                        exclude_init: bool = True, rrf_k: int = 60) -> Dataset:
    """LambdaRank 후보에서 ``__init__.py``를 빼고, 선택적으로 top-k를 적용한다.

    기본값은 나머지 전체 후보를 유지한다. ``top_k``를 주면 각 검색기의 상위
    ``top_k``를 합친 뒤 RRF로 다시 ``top_k``개를 고른다. 후보 선택에는 라벨을
    전혀 쓰지 않는다. ``ds.scanned``는 원래 스캔 집합 그대로 보존해, 후보에서
    잘린 gold가 recall ceiling에서 사라지는 평가 오류를 막는다.

    ``__init__.py``는 검색 결과로서 정보량이 낮으므로 후보 순위를 매기기 전에
    제외한다. top-k를 쓰지 않으면 그 밖의 파일은 모두 LambdaRank에 전달한다.
    """
    if top_k is not None and top_k <= 0:
        raise ValueError("top_k는 양수여야 한다")
    if rrf_k < 0:
        raise ValueError("rrf_k는 0 이상이어야 한다")

    required = {C.instance_id, C.file_id}
    if top_k is not None:
        required |= {C.bm25, C.ppr}
    missing = required - set(ds.features.columns)
    if missing:
        raise ValueError(f"후보 생성에 필요한 컬럼이 없다: {sorted(missing)}")

    before = len(ds.features)
    f = ds.features.copy()
    if exclude_init:
        basename = f[C.file_id].astype(str).str.replace("\\\\", "/", regex=False) \
                                           .str.rsplit("/", n=1).str[-1]
        f = f[basename != "__init__.py"].copy()
    after_init = len(f)

    empty_instances = set(ds.instance_ids) - set(f[C.instance_id])
    if empty_instances:
        raise SystemExit(
            f"__init__.py 제외 후 후보가 없는 인스턴스 {len(empty_instances)}개: "
            f"{sorted(empty_instances)[:5]}")

    if top_k is None:
        cand = f
    else:
        cand = _rrf_top_k(f, top_k, rrf_k)

    lost_instances = set(f[C.instance_id]) - set(cand[C.instance_id])
    if lost_instances:
        raise SystemExit(
            f"유효한 후보가 없는 인스턴스 {len(lost_instances)}개: "
            f"{sorted(lost_instances)[:5]}")

    retained_gold = sum(
        len(set(g[C.file_id]) & ds.gold_sets.get(iid, set()))
        for iid, g in cand.groupby(C.instance_id, sort=False))
    total_gold = sum(len(v) for v in ds.gold_sets.values())
    notes = list(ds.notes)
    if exclude_init:
        notes.append(f"__init__.py 후보 제외: {before - after_init}행")
    if top_k is None:
        notes.append(
            f"LambdaRank 전체 후보 유지: {before} → {len(cand)}행, "
            f"gold {retained_gold}/{total_gold} 유지")
    else:
        notes.append(
            f"BM25/PPR top-{top_k} 합집합 → RRF top-{top_k}: "
            f"{after_init} → {len(cand)}행, gold {retained_gold}/{total_gold} 유지")

    return Dataset(features=cand, gold_sets=ds.gold_sets, scanned=ds.scanned,
                   feature_columns=ds.feature_columns,
                   dropped_columns=ds.dropped_columns,
                   seg_gold=ds.seg_gold, notes=notes)


def _rrf_top_k(f: pd.DataFrame, top_k: int, rrf_k: int) -> pd.DataFrame:
    """BM25/PPR 각각의 top-k 합집합을 RRF로 정확히 top-k까지 줄인다."""
    both_missing = []
    for iid, g in f.groupby(C.instance_id, sort=False):
        if g[[C.bm25, C.ppr]].isna().all().all():
            both_missing.append(iid)
    if both_missing:
        raise SystemExit(
            f"BM25와 PPR이 모두 비어 있는 인스턴스 {len(both_missing)}개: "
            f"{sorted(both_missing)[:5]}")

    def exact_rank(col: str) -> pd.Series:
        ordered = f.sort_values(
            [C.instance_id, col, C.file_id],
            ascending=[True, False, True], na_position="last", kind="stable")
        values = ordered.groupby(C.instance_id, sort=False).cumcount().to_numpy() + 1
        return pd.Series(values, index=ordered.index).reindex(f.index)

    bm25_pos = exact_rank(C.bm25)
    ppr_pos = exact_rank(C.ppr)
    bm25_valid = f[C.bm25].notna()
    ppr_valid = f[C.ppr].notna()
    pool = ((bm25_pos <= top_k) & bm25_valid) | ((ppr_pos <= top_k) & ppr_valid)
    cand = f.loc[pool].copy()
    cand["__candidate_rrf"] = (
        np.where(bm25_valid.loc[cand.index],
                 1.0 / (rrf_k + bm25_pos.loc[cand.index].to_numpy(dtype=float)), 0.0)
        + np.where(ppr_valid.loc[cand.index],
                   1.0 / (rrf_k + ppr_pos.loc[cand.index].to_numpy(dtype=float)), 0.0))
    cand["__bm25_pos"] = bm25_pos.loc[cand.index].to_numpy(dtype=int)
    cand["__ppr_pos"] = ppr_pos.loc[cand.index].to_numpy(dtype=int)
    cand = cand.sort_values(
        [C.instance_id, "__candidate_rrf", "__bm25_pos", "__ppr_pos", C.file_id],
        ascending=[True, False, True, True, True], kind="stable")
    cand = cand.groupby(C.instance_id, sort=False).head(top_k)
    cand = cand.drop(columns=["__candidate_rrf", "__bm25_pos", "__ppr_pos"])
    cand = cand.sort_index().copy()

    return cand


def load_dataset(features_path: Path, manifest_path: Path,
                 *, bm25_seed_path: Path | None = None,
                 embed_seed_path: Path | None = None,
                 rerank_seed_path: Path | None = None,
                 extra_features_path: Path | None = None,
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

    if extra_features_path is not None:
        features, msg = join_extra(features, extra_features_path)
        notes.append(msg)

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
            + EXTRA_COLUMNS if c in features.columns]
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
    return per_query_minmax(df, columns, C.instance_id)
