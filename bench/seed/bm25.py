"""Okapi BM25 — 순수 Python/NumPy 구현.

`rank_bm25`가 이 환경에 없고, 외부 의존성을 새로 들이는 것보다 40줄짜리 표준
공식을 직접 두는 편이 재현 가능하다. 이건 **지표가 아니라 피처**이므로
`bench/metrics/` 자체구현 금지 규칙에 걸리지 않는다 (CONTRACT.md §2는 랭킹 지표 한정).

    idf(q)   = ln(1 + (N - df(q) + 0.5) / (df(q) + 0.5))
    score(d) = Σ_q  idf(q) · qw(q) · ( f(q,d)·(k1+1) )
                                     / ( f(q,d) + k1·(1 - b + b·|d|/avgdl) )

`qw(q)`는 쿼리 term frequency 가중치다.

  k3 = None     → 선형 (qtf 그대로). rank_bm25 / Anserini(Lucene)의 동작.  ← **기본값**
  k3 = 0        → 이진 (쿼리에 몇 번 나오든 1)
  0 < k3 < ∞    → 포화

기본값은 **표준 구현과 동일한 선형**이다. 처음에는 "problem_statement가 코드 덤프를
포함한 긴 문서라 반복 토큰이 쿼리를 지배한다"는 이유로 이진(k3=0)을 기본으로 뒀는데,
근거 없는 표준 이탈이었고 실제로 baseline을 크게 과소평가했다
(pydata/xarray 110 인스턴스: MRR 0.324 → 0.435, Recall@10 0.530 → 0.604).
BM25 baseline을 약하게 잡으면 이후 모든 개선폭이 부풀려진다.

k1 / b / k3의 **탐색**은 여기서 하지 않는다. 실제 튜닝은 반드시
**train 내부 CV로만** 한다 (CONTRACT.md §3-4). 위 수치는 표준 이탈을 되돌리기 위한
근거일 뿐 선택을 위한 탐색이 아니다.
"""

from __future__ import annotations

import math
from collections import Counter
from dataclasses import dataclass, field
from typing import Sequence

K1_DEFAULT = 1.2
B_DEFAULT = 0.75
K3_DEFAULT: float | None = None  # None = 선형 (표준). 0 = 이진.


@dataclass
class BM25:
    """문서 집합 하나에 대한 BM25 인덱스.

    SWE-bench는 인스턴스마다 corpus(=그 커밋 시점 repo 파일들)가 다르므로
    인스턴스당 인덱스를 하나씩 만든다. pytest 규모(~700 파일)에서는 비용이 무시된다.
    """

    doc_ids: list[str]
    k1: float = K1_DEFAULT
    b: float = B_DEFAULT
    k3: float | None = K3_DEFAULT

    _tf: list[Counter] = field(default_factory=list, repr=False)
    _len: list[int] = field(default_factory=list, repr=False)
    _df: Counter = field(default_factory=Counter, repr=False)
    _avgdl: float = 0.0

    @classmethod
    def build(cls, doc_ids: Sequence[str], docs: Sequence[Sequence[str]],
              *, k1: float = K1_DEFAULT, b: float = B_DEFAULT,
              k3: float | None = K3_DEFAULT) -> "BM25":
        if len(doc_ids) != len(docs):
            raise ValueError(f"doc_ids({len(doc_ids)}) != docs({len(docs)})")
        idx = cls(doc_ids=list(doc_ids), k1=k1, b=b, k3=k3)
        for toks in docs:
            c = Counter(toks)
            idx._tf.append(c)
            idx._len.append(len(toks))
            idx._df.update(c.keys())
        n = len(idx._len)
        idx._avgdl = (sum(idx._len) / n) if n else 0.0
        return idx

    @property
    def n_docs(self) -> int:
        return len(self.doc_ids)

    def idf(self, term: str) -> float:
        """Lucene식 평활화 IDF. df가 커도 음수가 되지 않는다.

        고전 Robertson IDF는 df > N/2에서 음수가 되어, 흔한 단어를 포함한 문서를
        **감점**한다. 700개 파일 중 400개에 나오는 `def` 같은 토큰에서 실제로
        발생하므로 반드시 평활화형을 쓴다.
        """
        df = self._df.get(term, 0)
        return math.log(1.0 + (self.n_docs - df + 0.5) / (df + 0.5))

    def _qweight(self, qtf: int) -> float:
        if self.k3 is None:
            return float(qtf)          # 선형 — 표준 구현과 동일
        if self.k3 <= 0:
            return 1.0                 # 이진
        return (self.k3 + 1.0) * qtf / (self.k3 + qtf)

    def score_query(self, query_tokens: Sequence[str]) -> list[float]:
        """모든 문서에 대한 BM25 점수. `doc_ids`와 같은 순서로 반환한다."""
        qtf = Counter(query_tokens)
        scores = [0.0] * self.n_docs
        if not self.n_docs or self._avgdl <= 0:
            return scores

        for term, qn in qtf.items():
            df = self._df.get(term, 0)
            if df == 0:
                continue
            w = self.idf(term) * self._qweight(qn)
            for i, tf in enumerate(self._tf):
                f = tf.get(term, 0)
                if not f:
                    continue
                denom = f + self.k1 * (1.0 - self.b + self.b * self._len[i] / self._avgdl)
                scores[i] += w * f * (self.k1 + 1.0) / denom
        return scores
