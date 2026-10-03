"""BM25 seed → Personalized PageRank teleport 벡터.

`algo-core`와의 계약 형태는 **`(file_id, weight)` 목록**이다.
PPR이 성립하려면 teleport 분포가 아래를 만족해야 한다.

  1. 모든 weight ≥ 0            — 음수는 확률분포가 아니다
  2. Σ weight == 1.0            — 정규화 실패 시 PageRank가 발산/수축한다
  3. file_id 가 전부 그래프 안에 존재 — 없는 노드로 텔레포트하면 질량이 증발한다

3번이 조용한 실패의 온상이다. BM25 corpus는 트리의 모든 `.py`인 반면 그래프는
파서가 성공한 파일만 담는다. 그래서 **누락 노드를 버리고 재정규화하되, 버린 질량을
반드시 보고**한다. 질량이 크면 seed가 그래프 밖을 가리키고 있다는 뜻이다.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from bench.schema import normalize_file_id

TOP_K_DEFAULT = 20


@dataclass
class TeleportVector:
    pairs: list[tuple[str, float]]
    dropped_file_ids: list[str] = field(default_factory=list)
    dropped_mass: float = 0.0
    fallback_uniform: bool = False

    @property
    def total(self) -> float:
        return sum(w for _, w in self.pairs)


class TeleportError(ValueError):
    """teleport 벡터가 PPR 입력으로 성립하지 않을 때."""


def build_teleport(scored: list[tuple[str, float]],
                   graph_file_ids: set[str] | None = None,
                   *, top_k: int | None = TOP_K_DEFAULT,
                   temperature: float = 1.0) -> TeleportVector:
    """BM25 점수를 teleport 분포로 변환한다.

    Args:
        scored: `(file_id, bm25_score)` 목록. 점수는 음수가 아니어야 한다
            (Lucene식 평활화 IDF를 쓰므로 BM25 값은 항상 ≥ 0이다).
        graph_file_ids: 그래프에 실제로 존재하는 노드. 주면 그 밖의 항목을
            버리고 재정규화하며, 버린 질량을 기록한다.
        top_k: 상위 k개만 남긴다. None이면 전부. 꼬리를 남기면 점수 0인 수백 개
            파일이 균등 잡음으로 들어와 personalization이 희석된다.
        temperature: `score ** (1/temperature)`. 1.0이면 그대로.
            <1 이면 상위에 더 몰리고, >1 이면 평탄해진다.

    Returns:
        TeleportVector. 유효한 항목이 하나도 없으면 그래프 전체 균등분포로
        폴백하고 `fallback_uniform=True`로 표시한다 — PPR에 영벡터를 넘기면
        결과가 정의되지 않기 때문이다.
    """
    cleaned: list[tuple[str, float]] = []
    for fid, s in scored:
        f = normalize_file_id(fid)
        v = float(s)
        if v != v:  # NaN
            continue
        if v < 0:
            raise TeleportError(f"음수 BM25 점수: {f}={v}")
        cleaned.append((f, v))

    dropped: list[str] = []
    # 버린 질량은 재정규화 **전** 점수 합 기준으로 센다
    total_all = sum(v for _, v in cleaned)
    dropped_sum = 0.0
    if graph_file_ids is not None:
        kept: list[tuple[str, float]] = []
        for f, v in cleaned:
            if f in graph_file_ids:
                kept.append((f, v))
            else:
                dropped.append(f)
                dropped_sum += v
        cleaned = kept

    cleaned = [(f, v) for f, v in cleaned if v > 0.0]
    cleaned.sort(key=lambda t: (-t[1], t[0]))
    if top_k is not None:
        cleaned = cleaned[:top_k]

    if temperature != 1.0:
        if temperature <= 0:
            raise TeleportError("temperature는 > 0 이어야 한다")
        cleaned = [(f, v ** (1.0 / temperature)) for f, v in cleaned]

    total = sum(v for _, v in cleaned)
    if total <= 0.0:
        # BM25가 전부 0 — 쿼리 토큰이 corpus에 하나도 없는 경우.
        pool = sorted(graph_file_ids) if graph_file_ids else [f for f, _ in cleaned]
        if not pool:
            raise TeleportError("teleport 후보가 하나도 없다 (그래프도 비었음)")
        w = 1.0 / len(pool)
        return TeleportVector(pairs=[(f, w) for f in pool],
                              dropped_file_ids=dropped,
                              dropped_mass=(dropped_sum / total_all) if total_all else 0.0,
                              fallback_uniform=True)

    pairs = [(f, v / total) for f, v in cleaned]
    return TeleportVector(pairs=pairs,
                          dropped_file_ids=dropped,
                          dropped_mass=(dropped_sum / total_all) if total_all > 0 else 0.0)


def validate_teleport(tv: TeleportVector,
                      graph_file_ids: set[str] | None = None,
                      *, tol: float = 1e-9) -> None:
    """PPR 입력 계약 3조건을 강제한다. 위반이면 TeleportError."""
    if not tv.pairs:
        raise TeleportError("빈 teleport 벡터")
    seen = set()
    for f, w in tv.pairs:
        if w < 0:
            raise TeleportError(f"음수 weight: {f}={w}")
        if w != w:
            raise TeleportError(f"NaN weight: {f}")
        if f in seen:
            raise TeleportError(f"중복 file_id: {f}")
        seen.add(f)
        if graph_file_ids is not None and f not in graph_file_ids:
            raise TeleportError(f"그래프에 없는 file_id: {f}")
    total = tv.total
    if abs(total - 1.0) > tol:
        raise TeleportError(f"weight 합이 1.0이 아니다: {total!r}")
