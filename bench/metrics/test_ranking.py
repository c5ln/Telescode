"""손 계산 fixture 테스트 — CONTRACT.md §2, ranking.py 모듈 docstring §2.

이 실험 최대의 위험은 "파이프라인은 완벽히 도는데 숫자가 틀린 것"이다.
따라서 기대값은 **구현을 호출하지 않고 손으로 유도한 상수**로 박는다.
`math.log2`를 기대값 쪽에서 다시 호출하면 log 밑 실수를 못 잡으므로
NDCG 기대값은 소수 리터럴로 고정한다.

    python3 -m pytest bench/metrics/test_ranking.py -q

각 케이스에 "무엇을 잡는 테스트인지"를 주석으로 남긴다. 그게 없으면
나중에 숫자가 깨졌을 때 기대값을 고쳐서 통과시키는 유혹에 진다.
"""

import math

import pytest

from bench.metrics.ranking import (
    apply_pessimistic_tiebreak,
    evaluate_instance,
    mrr,
    ndcg_at_k,
    reachable_recall_ceiling,
    recall_at_k,
)

NAN = float("nan")

# 자주 쓰는 IDCG@2 = 1/log2(2) + 1/log2(3) = 1 + 0.63092975357145743...
IDCG2 = 1.6309297535714575


def isnan(x) -> bool:
    return isinstance(x, float) and math.isnan(x)


# ══════════════════════════════════════════════════════════════════════════
# A. 동점 없는 기본 케이스 — 순위 인덱싱(1-base) 검증
#    ranked = [a, b, c, d, e], gold = {b, d}
#    gold 위치: b=2위, d=4위
# ══════════════════════════════════════════════════════════════════════════

A_RANKED = ["a", "b", "c", "d", "e"]
A_GOLD = {"b", "d"}


@pytest.mark.parametrize("k,expected", [
    (1, 0.0),        # 1위 a는 gold 아님
    (2, 0.5),        # b 하나 = 1/2
    (3, 0.5),
    (4, 1.0),        # b, d 둘 다 = 2/2
    (5, 1.0),
    (99, 1.0),       # k가 후보 수보다 크면 후보 수로 클램프
])
def test_recall_basic(k, expected):
    assert recall_at_k(A_RANKED, A_GOLD, k) == expected


def test_mrr_basic():
    # 첫 gold b가 2위 → 1/2
    assert mrr(A_RANKED, A_GOLD) == 0.5


def test_ndcg_basic():
    # DCG@3 = 1/log2(2+1)          (b가 2위)      = 0.63092975357145743
    # IDCG@3 = min(3, |gold|=2)개  = 1 + 0.63092975357145743
    # NDCG@3 = 0.63092975.../1.63092975... = 0.38685280723454163
    assert ndcg_at_k(A_RANKED, A_GOLD, 3) == pytest.approx(0.38685280723454163)


def test_ndcg_perfect_when_gold_on_top():
    # gold가 정확히 상위에 몰리면 1.0. log 밑이나 오프셋이 틀리면 깨진다.
    assert ndcg_at_k(["b", "d", "a", "c"], A_GOLD, 4) == pytest.approx(1.0)


# ══════════════════════════════════════════════════════════════════════════
# B. 비관적 동점 처리 — 이 실험에서 가장 중요한 계약
# ══════════════════════════════════════════════════════════════════════════

def test_tiebreak_pushes_gold_to_back_of_tie_group():
    # a, b, c가 전부 1.0 동점이고 a만 gold.
    # 비관적: 동점 그룹 안에서 gold를 뒤로 → [b, c, a, d]
    # 낙관적이라면 [a, b, c, d]가 되어 MRR이 1.0으로 부풀었을 것이다.
    ranked = apply_pessimistic_tiebreak(
        ["a", "b", "c", "d"], [1.0, 1.0, 1.0, 0.5], {"a"})
    assert ranked == ["b", "c", "a", "d"]
    assert mrr(ranked, {"a"}) == pytest.approx(1.0 / 3.0)
    assert recall_at_k(ranked, {"a"}, 2) == 0.0
    assert recall_at_k(ranked, {"a"}, 3) == 1.0


def test_tiebreak_all_scores_zero():
    # F3 재현: 전 후보가 0점인 구간. 유일한 gold가 맨 뒤로 가야 한다.
    # 이 케이스가 낙관적으로 처리되면 baseline 점수가 통째로 거짓이 된다.
    ids = ["a", "b", "c", "d", "e"]
    ranked = apply_pessimistic_tiebreak(ids, [0.0] * 5, {"a"})
    assert ranked == ["b", "c", "d", "e", "a"]
    assert mrr(ranked, {"a"}) == pytest.approx(0.2)
    assert recall_at_k(ranked, {"a"}, 4) == 0.0
    assert recall_at_k(ranked, {"a"}, 5) == 1.0
    # DCG = 1/log2(5+1), IDCG = 1/log2(1+1) = 1  →  0.38685280723454163
    assert ndcg_at_k(ranked, {"a"}, 5) == pytest.approx(0.38685280723454163)


def test_tiebreak_is_deterministic_regardless_of_input_order():
    # 입력 순서가 결과를 바꾸면 재현이 안 된다. file_id 3차 키가 이걸 막는다.
    gold = {"a"}
    first = apply_pessimistic_tiebreak(["d", "c", "b", "a"], [1, 1, 1, 1], gold)
    second = apply_pessimistic_tiebreak(["a", "b", "c", "d"], [1, 1, 1, 1], gold)
    assert first == second == ["b", "c", "d", "a"]


def test_tiebreak_respects_score_order_across_groups():
    # 비관적 처리는 동점 **안에서만** 작동한다. 점수가 다르면 gold가 앞에 온다.
    ranked = apply_pessimistic_tiebreak(
        ["a", "b", "c"], [0.9, 0.5, 0.1], {"a"})
    assert ranked == ["a", "b", "c"]


def test_tiebreak_nan_score_goes_last():
    # NaN은 비교에서 조용히 순서를 깨뜨린다. 최하위로 몰아야 한다.
    ranked = apply_pessimistic_tiebreak(["a", "b", "c"], [NAN, 1.0, 0.0], set())
    assert ranked == ["b", "c", "a"]


def test_tiebreak_length_mismatch_raises():
    with pytest.raises(ValueError):
        apply_pessimistic_tiebreak(["a", "b"], [1.0], set())


# ══════════════════════════════════════════════════════════════════════════
# C. IDCG 상한 — gold가 k보다 많을 때
# ══════════════════════════════════════════════════════════════════════════

def test_ndcg_idcg_capped_at_k():
    # gold 4개, k=2. 상위 2개가 전부 gold면 NDCG@2는 정확히 1.0이어야 한다.
    # IDCG를 gold 4개로 계산하면 1보다 작아지고, k개로 안 자르면 상한이 1을 넘는다.
    ranked = ["a", "b", "c", "d", "e"]
    gold = {"a", "b", "c", "d"}
    assert ndcg_at_k(ranked, gold, 2) == pytest.approx(1.0)
    # 같은 상황에서 Recall@2는 2/4 = 0.5다. 둘이 다른 게 정상.
    assert recall_at_k(ranked, gold, 2) == 0.5


# ══════════════════════════════════════════════════════════════════════════
# D. 분모 규약 — 스캔에 없는 gold도 분모에 남는다
# ══════════════════════════════════════════════════════════════════════════

def test_unreachable_gold_stays_in_denominator():
    # z는 스캔 결과에 아예 없다. 분모를 gold∩scanned로 잡으면 recall이 1.0으로
    # 부풀고 ceiling이 의미를 잃는다.
    ranked = ["a", "b"]
    gold = {"a", "z"}
    assert recall_at_k(ranked, gold, 10) == 0.5
    # DCG = 1/log2(1+1) = 1.0, IDCG = min(10, 2) = 2개 = 1.6309297535714575
    assert ndcg_at_k(ranked, gold, 10) == pytest.approx(0.6131471927654584)
    assert reachable_recall_ceiling({"a", "b"}, gold) == 0.5


def test_reachable_recall_ceiling():
    assert reachable_recall_ceiling({"a", "b", "c"}, {"a", "b"}) == 1.0
    assert reachable_recall_ceiling({"a", "b", "c"}, {"a", "z"}) == 0.5
    assert reachable_recall_ceiling({"a"}, {"y", "z"}) == 0.0
    assert reachable_recall_ceiling(set(), {"a"}) == 0.0


def test_recall_is_bounded_by_ceiling():
    # 계약의 핵심 성질: Recall@k ≤ ceiling. k를 무한대로 키워도 넘을 수 없다.
    scanned = ["a", "b", "c"]
    gold = {"a", "z"}
    ceiling = reachable_recall_ceiling(set(scanned), gold)
    for k in (1, 2, 3, 100):
        assert recall_at_k(scanned, gold, k) <= ceiling + 1e-12


# ══════════════════════════════════════════════════════════════════════════
# E. gold가 비었거나 하나도 못 찾은 경우
# ══════════════════════════════════════════════════════════════════════════

def test_empty_gold_returns_nan_not_zero():
    # 0.0으로 두면 평균이 왜곡된다. 집계는 nanmean으로 해야 한다.
    assert isnan(recall_at_k(["a", "b"], set(), 5))
    assert isnan(mrr(["a", "b"], set()))
    assert isnan(ndcg_at_k(["a", "b"], set(), 5))
    assert isnan(reachable_recall_ceiling({"a"}, set()))


def test_no_gold_found_is_zero_not_nan():
    # gold는 있는데 후보에 없다 → 0.0. NaN과 구분돼야 한다.
    ranked = ["a", "b", "c"]
    gold = {"z"}
    assert mrr(ranked, gold) == 0.0
    assert recall_at_k(ranked, gold, 3) == 0.0
    assert ndcg_at_k(ranked, gold, 3) == 0.0


def test_empty_candidates():
    assert recall_at_k([], {"a"}, 5) == 0.0
    assert mrr([], {"a"}) == 0.0
    assert ndcg_at_k([], {"a"}, 5) == 0.0


def test_k_zero_or_negative():
    assert recall_at_k(A_RANKED, A_GOLD, 0) == 0.0
    assert ndcg_at_k(A_RANKED, A_GOLD, 0) == 0.0
    assert recall_at_k(A_RANKED, A_GOLD, -1) == 0.0


# ══════════════════════════════════════════════════════════════════════════
# F. 중복 file_id 방어
# ══════════════════════════════════════════════════════════════════════════

def test_duplicate_file_ids_counted_once():
    # 조인 실수로 같은 file_id가 두 번 들어오면 recall이 1을 넘을 수 있다.
    ranked = ["a", "a", "b"]
    gold = {"a", "b"}
    assert recall_at_k(ranked, gold, 2) == 0.5     # a 한 번만 센다
    assert recall_at_k(ranked, gold, 3) == 1.0
    # DCG: 1위 a = 1.0, 2위 a는 중복이라 0, 3위 b = 1/log2(3+1) = 0.5 → 1.5
    # IDCG@3 = min(3, 2) = 2개 = 1.6309297535714575
    assert ndcg_at_k(ranked, gold, 3) == pytest.approx(0.9197207891481876)


# ══════════════════════════════════════════════════════════════════════════
# G. evaluate_instance 통합 — 소비자가 tiebreak을 잊어도 안전한가
# ══════════════════════════════════════════════════════════════════════════

def test_evaluate_instance_applies_tiebreak_internally():
    # 점수 순서대로 넣었을 때 내부에서 비관적 정렬이 적용돼야 한다.
    # a(gold)가 b, c와 동점이므로 3위로 밀린다 → mrr = 1/3
    out = evaluate_instance(
        ["a", "b", "c", "d"], [1.0, 1.0, 1.0, 0.5], {"a"}, ks=(1, 3))
    assert out["mrr"] == pytest.approx(1.0 / 3.0)
    assert out["recall@1"] == 0.0
    assert out["recall@3"] == 1.0
    assert out["n_candidates"] == 4
    assert out["n_gold"] == 1
    assert out["reachable_recall_ceiling"] == 1.0


def test_evaluate_instance_reports_ceiling_below_one():
    out = evaluate_instance(["a", "b"], [1.0, 0.5], {"a", "z"}, ks=(10,))
    assert out["reachable_recall_ceiling"] == 0.5
    assert out["recall@10"] == 0.5


def test_pessimistic_never_scores_above_optimistic():
    # 방향성 회귀 가드: 같은 점수 벡터에서 비관적 MRR ≤ 낙관적 MRR.
    ids = ["a", "b", "c", "d", "e"]
    scores = [1.0, 1.0, 1.0, 1.0, 0.0]
    gold = {"c"}
    pess = mrr(apply_pessimistic_tiebreak(ids, scores, gold), gold)
    optimistic = [f for f, _ in sorted(
        zip(ids, scores), key=lambda p: (-p[1], p[0] not in gold, p[0]))]
    assert pess <= mrr(optimistic, gold)
    assert pess == pytest.approx(0.25)   # c가 4위 (a, b, d 뒤)


# ══════════════════════════════════════════════════════════════════════════
# H. 실제로 저질렀던 버그의 회귀 가드
#    피처 매트릭스에서 gold를 복원하면 스캔이 놓친 gold가 조용히 사라진다.
#    → Recall 분모가 줄어 점수가 부풀고, ceiling은 항상 1.0이 되어
#      "상한을 함께 보고하라"는 계약이 무의미해진다.
# ══════════════════════════════════════════════════════════════════════════

def test_gold_recovered_from_matrix_would_inflate_recall():
    scanned = ["a", "b", "c"]          # 스캔된 후보
    true_gold = {"a", "z"}             # z는 patch가 새로 만든 파일 → 스캔에 없음
    gold_from_matrix = true_gold & set(scanned)   # 잘못된 복원 방식

    # 잘못된 방식은 만점을 준다.
    assert recall_at_k(scanned, gold_from_matrix, 10) == 1.0
    assert reachable_recall_ceiling(set(scanned), gold_from_matrix) == 1.0

    # 올바른 분모는 절반으로 깎고, ceiling이 그 이유를 설명한다.
    assert recall_at_k(scanned, true_gold, 10) == 0.5
    assert reachable_recall_ceiling(set(scanned), true_gold) == 0.5


def test_baseline_gold_sets_come_from_manifest():
    import pandas as pd

    from bench.metrics.baseline import gold_sets_from_manifest

    manifest = pd.DataFrame([
        {"instance_id": "i1", "status": "ok", "gold_files": "a.py;z.py"},
        {"instance_id": "i2", "status": "ok", "gold_files": "b.py"},
        {"instance_id": "i3", "status": "failed", "gold_files": ""},
    ])
    gold = gold_sets_from_manifest(manifest)
    assert gold == {"i1": {"a.py", "z.py"}, "i2": {"b.py"}}   # 실패 인스턴스는 제외
