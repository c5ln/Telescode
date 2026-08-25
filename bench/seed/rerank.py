"""크로스 인코더 리랭킹 — retrieve-then-rerank 3단계.

    1단계  BM25 / 임베딩으로 후보 생성      (bi-encoder, 문서를 미리 인코딩)
    2단계  상위 N개만 크로스 인코더로 재채점  ← 이 모듈
    3단계  LTR이 전부를 결합

## 왜 크로스 인코더인가

바이 인코더(임베딩)는 쿼리와 문서를 **따로** 벡터로 만든 뒤 코사인을 잰다.
그래서 문서를 미리 인코딩해 캐시할 수 있지만(6,308 blob을 한 번만), 쿼리와
문서가 서로를 보지 못한다.

크로스 인코더는 (쿼리, 문서)를 **함께** 넣어 관련성을 직접 낸다. 정확도가 높은
대신 캐시가 원리상 불가능하다 — 점수가 쿼리에 종속되므로 인스턴스마다 다시
계산해야 한다. 그래서 전 후보(43,688쌍)가 아니라 **상위 N개만** 넘긴다.

## 이 실험에서의 질문

`RESULTS.md` §7이 확정한 것: 어휘가 겹치지 않는 구간에서 BM25는 ×0.33로 붕괴하고
임베딩도 ×0.50으로 떨어진다. 크로스 인코더는 **이슈와 코드를 같이 읽는** 유일한
방식이므로, 그 구간에서 오르는지가 이 단계의 핵심 질문이다.

## 후보 상한 (실측)

리랭커는 후보 밖 파일을 되살릴 수 없다. N이 곧 성능 상한이다:

    N     전체 recall   non_overlap
    10      0.776         0.555
    20      0.866         0.742
    30      0.900         0.805
    50      0.953         0.869      <- 채택
    100     0.991         0.953

## ⚠ 후보 집합이 임베딩에 종속된다

상위 N개도, 각 파일을 대표하는 청크도 임베딩이 고른다. 즉 `rerank`는 `embed`와
독립이 아니다. **`ppr`이 `bm25` seed에 종속됐던 것과 같은 구조**이며(§5·§9-2),
ablation에서 semantic 그룹 내부의 한계 기여를 해석할 때 반드시 감안해야 한다.
cascade의 본질적 성질이라 없앨 수는 없고, 명시하는 것으로 대신한다.
"""

from __future__ import annotations

import json
import os
import sys
import threading
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass

import numpy as np

from bench.env import load_dotenv, require
from bench.seed.api_encoder import DEFAULT_BASE_URL, ApiEncodeError, _Retry

DEFAULT_RERANK_MODEL = "voyageai/rerank-2.5"

#: 인스턴스당 리랭킹할 후보 파일 수. 위 "후보 상한" 표 참조.
TOP_N = 50

#: 동시 요청 수. 임베딩과 같은 이유(왕복 지연 지배).
RERANK_CONCURRENCY = 8


@dataclass
class RerankStats:
    n_queries: int = 0
    n_docs: int = 0
    n_tokens: int = 0        # API가 보고한 값. 없으면 0
    cost: float = 0.0        # API가 보고한 값
    seconds: float = 0.0

    def __str__(self) -> str:
        return (f"queries={self.n_queries} docs={self.n_docs:,} "
                f"tokens={self.n_tokens:,} cost=${self.cost:.4f} {self.seconds:.0f}s")


class OpenRouterReranker:
    """OpenRouter `/rerank` 래퍼.

    `api_encoder.OpenRouterEncoder`와 재시도·동시성 구조를 맞춘다.
    """

    def __init__(self, model: str = DEFAULT_RERANK_MODEL, *,
                 concurrency: int = RERANK_CONCURRENCY,
                 base_url: str = DEFAULT_BASE_URL,
                 api_key: str | None = None,
                 timeout: float = 120.0):
        self.model = model
        self.concurrency = max(1, concurrency)
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.retry = _Retry()
        self.stats = RerankStats()
        self._lock = threading.Lock()

        load_dotenv()
        self._api_key = api_key or os.environ.get("OPENROUTER_API_KEY") or None

    @property
    def api_key(self) -> str:
        if not self._api_key:
            self._api_key = require(
                "OPENROUTER_API_KEY", hint="키 발급: https://openrouter.ai/keys")
        return self._api_key

    def _post(self, query: str, documents: list[str]) -> np.ndarray:
        """(쿼리, 문서들) → 문서 순서대로 정렬된 relevance 배열."""
        body = json.dumps({"model": self.model, "query": query,
                           "documents": documents,
                           "top_n": len(documents)}).encode("utf-8")

        last: Exception | None = None
        for attempt in range(self.retry.tries):
            req = urllib.request.Request(
                f"{self.base_url}/rerank", data=body, method="POST",
                headers={"Authorization": f"Bearer {self.api_key}",
                         "Content-Type": "application/json"})
            try:
                with urllib.request.urlopen(req, timeout=self.timeout) as r:
                    doc = json.loads(r.read().decode("utf-8"))
                return self._scores_from(doc, len(documents))
            except urllib.error.HTTPError as e:
                detail = e.read().decode("utf-8", errors="replace")[:400]
                last = ApiEncodeError(f"HTTP {e.code}: {detail}")
                if e.code != 429 and e.code < 500:
                    raise last from e
            except (urllib.error.URLError, TimeoutError, OSError,
                    json.JSONDecodeError) as e:
                last = e
            # 재시도를 반드시 찍는다 — 임베딩 단계에서 조용한 정체로 30분을
            # 날린 적이 있다 (api_encoder._post 주석 참조).
            print(f"    retry {attempt + 1}/{self.retry.tries} "
                  f"(docs={len(documents)}): {type(last).__name__} {str(last)[:110]}",
                  file=sys.stderr, flush=True)
            time.sleep(min(self.retry.base ** attempt, self.retry.cap))
        raise ApiEncodeError(f"{self.retry.tries}회 재시도 실패: {last}")

    def _scores_from(self, doc: dict, n_expected: int) -> np.ndarray:
        results = doc.get("results")
        if not isinstance(results, list) or len(results) != n_expected:
            raise ApiEncodeError(
                f"응답 개수 불일치: 기대 {n_expected}, 실제 "
                f"{len(results) if isinstance(results, list) else type(results)}"
                f" / {str(doc)[:300]}")
        # 응답은 relevance 내림차순이다. **입력 순서로 되돌려야** 호출부가
        # 파일과 짝지을 수 있다. index를 신뢰하고 순서는 신뢰하지 않는다.
        out = np.zeros(n_expected, dtype=np.float32)
        seen = set()
        for r in results:
            i = int(r["index"])
            if not (0 <= i < n_expected) or i in seen:
                raise ApiEncodeError(f"index가 이상하다: {i} (n={n_expected})")
            seen.add(i)
            out[i] = float(r["relevance_score"])

        usage = doc.get("usage") or {}
        with self._lock:
            self.stats.n_tokens += int(usage.get("total_tokens") or 0)
            self.stats.cost += float(usage.get("cost") or 0.0)
        return out

    def rerank_batch(self, jobs: list[tuple[str, list[str]]]) -> list[np.ndarray]:
        """[(query, docs)] → [scores]. 입력 순서를 보존한다.

        `pool.map`은 순서를 지킨다 — 한 건이라도 자리가 바뀌면 인스턴스와 점수가
        어긋난 채로 조용히 끝난다.
        """
        if not jobs:
            return []
        t0 = time.time()
        fn = lambda qd: self._post(qd[0], qd[1])      # noqa: E731
        if self.concurrency == 1 or len(jobs) == 1:
            out = [fn(j) for j in jobs]
        else:
            with ThreadPoolExecutor(max_workers=self.concurrency) as pool:
                out = list(pool.map(fn, jobs))
        with self._lock:
            self.stats.n_queries += len(jobs)
            self.stats.n_docs += sum(len(d) for _, d in jobs)
            self.stats.seconds += time.time() - t0
        return out
