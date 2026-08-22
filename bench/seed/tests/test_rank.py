from bench.seed.run_seed import rank_pessimistic


def test_no_ties():
    assert rank_pessimistic([3.0, 1.0, 2.0]) == [1, 3, 2]


def test_ties_get_worst_rank():
    # 5.0 두 개 → 둘 다 2위(최악), 다음은 3위
    assert rank_pessimistic([5.0, 5.0, 1.0]) == [2, 2, 3]


def test_all_zero_all_worst():
    assert rank_pessimistic([0.0, 0.0, 0.0, 0.0]) == [4, 4, 4, 4]


def test_mass_tie_tail():
    # 실제 BM25 모습: 상위 소수 + 대량 0점 동점
    scores = [9.0, 4.0] + [0.0] * 8
    r = rank_pessimistic(scores)
    assert r[0] == 1 and r[1] == 2
    assert set(r[2:]) == {10}


def test_rank_does_not_use_labels():
    # 같은 점수 배열이면 gold가 무엇이든 결과가 동일해야 한다 (피처 누출 방지)
    a = rank_pessimistic([1.0, 1.0, 0.0])
    b = rank_pessimistic([1.0, 1.0, 0.0])
    assert a == b
