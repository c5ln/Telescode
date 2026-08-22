"""BM25 손 계산 fixture 테스트.

구현 버그는 파이프라인이 완벽하게 돌면서 '그럴듯한데 틀린 숫자'를 낸다.
그래서 공식을 종이에서 그대로 다시 계산해 비교한다.
"""
import math

from bench.seed.bm25 import BM25


def test_idf_never_negative_for_common_terms():
    # 4개 문서 중 4개에 등장 → 고전 Robertson IDF라면 음수가 된다.
    idx = BM25.build(["a", "b", "c", "d"], [["x"], ["x"], ["x"], ["x"]])
    assert idx.idf("x") > 0


def test_hand_computed_score():
    docs = {
        "d1": ["alpha", "alpha", "beta"],          # len 3
        "d2": ["alpha", "gamma"],                  # len 2
        "d3": ["gamma", "gamma", "gamma", "delta"],  # len 4
    }
    idx = BM25.build(list(docs), list(docs.values()), k1=1.2, b=0.75, k3=0.0)

    n = 3
    avgdl = (3 + 2 + 4) / 3
    assert idx._avgdl == avgdl

    # 손 계산: query = ["alpha"], df(alpha) = 2
    idf = math.log(1 + (n - 2 + 0.5) / (2 + 0.5))
    k1, b = 1.2, 0.75

    def s(f, dl):
        return idf * f * (k1 + 1) / (f + k1 * (1 - b + b * dl / avgdl))

    got = idx.score_query(["alpha"])
    assert math.isclose(got[0], s(2, 3), rel_tol=1e-12)
    assert math.isclose(got[1], s(1, 2), rel_tol=1e-12)
    assert got[2] == 0.0


def test_default_k3_is_linear():
    """기본값은 표준(선형)이어야 한다.

    이진(k3=0)으로 되돌리면 BM25 baseline이 과소평가되고, 그러면 이후 임베딩/LLM
    단계의 개선폭이 전부 부풀려진다 (pydata/xarray: Recall@10 0.604 -> 0.530).
    """
    from bench.seed.bm25 import K3_DEFAULT
    assert K3_DEFAULT is None
    idx = BM25.build(["d1"], [["alpha"] * 3])
    assert math.isclose(idx.score_query(["alpha", "alpha"])[0],
                        2 * idx.score_query(["alpha"])[0], rel_tol=1e-12)


def test_k3_binary_vs_linear():
    docs = [["alpha"] * 3, ["beta"]]
    idx_bin = BM25.build(["d1", "d2"], docs, k3=0.0)
    idx_lin = BM25.build(["d1", "d2"], docs, k3=None)
    q = ["alpha", "alpha", "alpha"]
    # 이진: 쿼리 반복이 점수를 바꾸지 않는다
    assert math.isclose(idx_bin.score_query(q)[0], idx_bin.score_query(["alpha"])[0])
    # 선형: 3배가 된다
    assert math.isclose(idx_lin.score_query(q)[0],
                        3 * idx_lin.score_query(["alpha"])[0], rel_tol=1e-12)


def test_unknown_term_scores_zero():
    idx = BM25.build(["d1"], [["alpha"]])
    assert idx.score_query(["zzz"]) == [0.0]


def test_empty_corpus():
    idx = BM25.build([], [])
    assert idx.score_query(["alpha"]) == []
