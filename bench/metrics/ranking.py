"""랭킹 지표 — 이 실험의 **유일한** 구현.

`retrieval`과 `ltr-eval`은 자체 구현을 만들지 말고 여기서 import 한다.
구현이 두 개면 숫자가 갈리고, 어느 쪽이 맞는지 알 수 없게 된다.

구현 담당: `bench-harness`
소비자:   `retrieval`, `ltr-eval`

## 반드시 지킬 것

1. **동점은 비관적으로 처리한다.** 동점 그룹에서 positive에게 최악 순위를 부여한다.
   낙관적(최선 순위) 처리는 점수를 부풀리고, 특히 combined_score가 대량 동점인
   구간(수정 전 baseline에서 382/680개가 0점이었다)에서 결과를 무의미하게 만든다.

2. **손 계산 fixture 테스트를 반드시 작성한다.** 지표 구현 버그는 파이프라인이
   완벽하게 돌아가면서 "그럴듯한데 틀린 숫자"를 내놓는다. 이 실험의 최대 위험이다.
   → `bench/metrics/test_ranking.py`

3. `reachable_recall_ceiling`을 항상 함께 보고한다. gold 파일이 스캔 결과에
   없으면 Recall 상한이 1 미만인데, 모르면 낮은 점수를 알고리즘 탓으로 오해한다.

## 분모 규약 (중요)

Recall / NDCG의 분모는 **`gold_file_ids` 전체**다. 스캔에 없는 gold도 분모에 남긴다.
그래야 `reachable_recall_ceiling`이 실제로 상한 역할을 한다.
분모를 `gold ∩ scanned`로 잡으면 파서가 못 읽은 파일이 조용히 사라져
점수가 부풀고 ceiling은 의미를 잃는다.

## NaN 규약

gold가 빈 인스턴스는 `float('nan')`을 반환한다. 0.0으로 두면 평균이 왜곡된다.
집계 시 반드시 `numpy.nanmean` 계열을 쓰고, NaN 개수를 함께 보고할 것.
"""

import math
from typing import Sequence


def _distinct_hits(ranked_file_ids: Sequence[str],
                   gold_file_ids: set[str],
                   k: int) -> int:
    """상위 k개 안의 **서로 다른** gold 개수.

    ranked에 중복 file_id가 들어와도 같은 gold를 두 번 세지 않는다.
    """
    seen: set[str] = set()
    for fid in ranked_file_ids[:k]:
        if fid in gold_file_ids:
            seen.add(fid)
    return len(seen)


def recall_at_k(ranked_file_ids: Sequence[str],
                gold_file_ids: set[str],
                k: int) -> float:
    """상위 k개 안에 든 gold 파일의 비율.

    Args:
        ranked_file_ids: 점수 내림차순 정렬된 file_id. 동점은 호출 전에
            비관적 순서(positive를 뒤로)로 정렬돼 있어야 한다.
            → `apply_pessimistic_tiebreak`
        gold_file_ids: 정답 file_id 집합.
        k: 절단 지점. 후보 수보다 크면 후보 수로 클램프한다.

    Returns:
        [0, 1]. gold가 비면 float('nan') — 0.0으로 두면 평균이 왜곡된다.

    분모는 `len(gold_file_ids)`다 (모듈 docstring의 분모 규약 참조).
    """
    if not gold_file_ids:
        return float("nan")
    if k <= 0:
        return 0.0
    k = min(k, len(ranked_file_ids))
    return _distinct_hits(ranked_file_ids, gold_file_ids, k) / len(gold_file_ids)


def mrr(ranked_file_ids: Sequence[str], gold_file_ids: set[str]) -> float:
    """첫 gold 파일 순위의 역수. gold가 하나도 안 나오면 0.0."""
    if not gold_file_ids:
        return float("nan")
    for i, fid in enumerate(ranked_file_ids, start=1):
        if fid in gold_file_ids:
            return 1.0 / i
    return 0.0


def ndcg_at_k(ranked_file_ids: Sequence[str],
              gold_file_ids: set[str],
              k: int) -> float:
    """이진 relevance NDCG@k.

    IDCG는 **실제 gold 개수와 k 중 작은 값**으로 계산한다.
    gold가 k보다 많을 때 IDCG를 k개로 고정하지 않으면 상한이 1을 넘는다.

    분모(IDCG)가 `min(k, len(gold_file_ids))`이므로 스캔에 없는 gold도 분모에
    남는다. Recall과 같은 규약이다.
    """
    if not gold_file_ids:
        return float("nan")
    if k <= 0:
        return 0.0

    cut = min(k, len(ranked_file_ids))
    seen: set[str] = set()
    dcg = 0.0
    for i, fid in enumerate(ranked_file_ids[:cut], start=1):
        if fid in gold_file_ids and fid not in seen:
            seen.add(fid)
            dcg += 1.0 / math.log2(i + 1)

    ideal_n = min(k, len(gold_file_ids))
    idcg = sum(1.0 / math.log2(i + 1) for i in range(1, ideal_n + 1))
    if idcg == 0.0:
        return float("nan")
    return dcg / idcg


def reachable_recall_ceiling(scanned_file_ids: set[str],
                             gold_file_ids: set[str]) -> float:
    """스캔 결과에 실제로 존재하는 gold 파일의 비율 = Recall의 상한.

    파서가 못 읽은 파일, 삭제된 파일, `.py`가 아닌 파일 때문에 1 미만이 된다.
    이 값을 모르면 "Recall@10이 0.4밖에 안 된다"를 알고리즘 탓으로 오해한다.
    """
    if not gold_file_ids:
        return float("nan")
    return len(gold_file_ids & scanned_file_ids) / len(gold_file_ids)


def apply_pessimistic_tiebreak(file_ids: Sequence[str],
                               scores: Sequence[float],
                               gold_file_ids: set[str]) -> list[str]:
    """점수 내림차순 정렬하되, 동점 그룹 안에서는 gold를 뒤로 보낸다.

    모든 지표 함수 호출 전에 이걸 통과시킨다. 위 §1 참조.

    정렬 키는 `(-score, is_gold, file_id)`다.
      - `-score`  : 내림차순
      - `is_gold` : 동점 그룹 안에서 gold(1)를 non-gold(0) 뒤로
      - `file_id` : 남은 동점의 결정론적 순서. 이게 없으면 같은 입력이
                    실행마다 다른 순서를 내서 재현이 안 된다.

    NaN 점수는 최하위로 보낸다 (NaN은 비교에서 조용히 순서를 깨뜨린다).
    """
    if len(file_ids) != len(scores):
        raise ValueError(
            f"file_ids({len(file_ids)})와 scores({len(scores)}) 길이가 다르다"
        )

    def key(pair):
        fid, score = pair
        s = float(score)
        if math.isnan(s):
            # NaN은 최하위. -inf를 쓰면 -score가 +inf가 되어 맨 앞으로 간다.
            return (float("inf"), 1 if fid in gold_file_ids else 0, fid)
        return (-s, 1 if fid in gold_file_ids else 0, fid)

    return [fid for fid, _ in sorted(zip(file_ids, scores), key=key)]


def evaluate_instance(file_ids: Sequence[str],
                      scores: Sequence[float],
                      gold_file_ids: set[str],
                      ks: Sequence[int] = (1, 3, 5, 10, 20, 50)) -> dict:
    """한 인스턴스의 전 지표를 한 번에 계산한다.

    소비자가 tiebreak 적용을 잊는 것을 막기 위한 편의 함수다.
    `file_ids`는 스캔된 후보 전체여야 한다 (ceiling 계산에 쓰인다).
    """
    ranked = apply_pessimistic_tiebreak(file_ids, scores, gold_file_ids)
    out = {
        "n_candidates": len(ranked),
        "n_gold": len(gold_file_ids),
        "mrr": mrr(ranked, gold_file_ids),
        "reachable_recall_ceiling": reachable_recall_ceiling(
            set(file_ids), gold_file_ids),
    }
    for k in ks:
        out[f"recall@{k}"] = recall_at_k(ranked, gold_file_ids, k)
        out[f"ndcg@{k}"] = ndcg_at_k(ranked, gold_file_ids, k)
    return out
