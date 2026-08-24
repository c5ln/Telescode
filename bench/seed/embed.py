"""청크 단위 임베딩 — PPR seed 독립성 확보가 주 목적.

## 왜 임베딩인가 (순위 경쟁이 아니다)

`RESULTS.md` §7-6의 한계: PPR seed를 BM25에서 뽑았기 때문에 `ppr` 피처와 `bm25`
피처가 독립이 아니고, 그래서 ablation에서 graph 그룹의 한계 기여가 유의하지 않았다
(-0.020, p=0.074).

임베딩은 **BM25와 다른 신호원**이다. 그걸로 만든 seed로 PPR을 돌리면 구조 신호와
의미 신호의 중복이 줄어든다. 그러면 "구조가 독립적으로 기여한다"를 처음으로
말할 수 있다. **임베딩이 BM25보다 약해도 이 목적에는 문제가 없다.**

## 청크 단위인 이유

파일을 통째로 한 벡터에 넣으면 긴 파일에서 신호가 희석된다. `dataset.py`(수천 줄)
안의 관련 함수 하나가 전체 평균에 묻힌다. 그래서 토큰 윈도로 잘라 청크마다
벡터를 만들고 **최대**를 취한다 — "이 파일 어딘가에 이슈와 맞는 부분이 있는가".

집계는 두 가지를 다 낸다.

  `maxsim`  : score = max_c cos(q, c)          ← 기본값
  `maxpool` : file_vec = 차원별 max, 재정규화 후 cos(q, file_vec)

`maxsim`을 기본으로 둔 이유: 차원별 max는 어떤 실제 청크에도 대응하지 않는
합성 벡터를 만든다. 정규화된 임베딩 공간에서 그건 기하학적 의미가 없고,
차원마다 다른 청크에서 값을 가져오므로 "가장 잘 맞는 부분"이라는 해석이 깨진다.
`maxsim`은 점수 수준에서 max를 취해 그 해석을 유지한다(late interaction과 같은 형태).
둘 다 산출하므로 차이는 보고서에서 비교한다.

## 캐시

blob SHA로 청크 벡터를 캐시한다. 229 인스턴스의 (instance, file) 쌍은 43,688개인데
고유 blob은 6,308개다 — **85.6%가 중복**이다. 캐시가 없으면 같은 파일을 수십 번
인코딩한다.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

DEFAULT_MODEL = "sentence-transformers/all-MiniLM-L6-v2"

MAX_LEN = 256        # 청크당 토큰 수 (특수토큰 제외 254)
STRIDE = 192         # 윈도 이동폭. 64 토큰 겹침으로 경계에 걸친 함수를 살린다
MAX_CHUNKS = 64      # 파일당 상한. 64×192 ≈ 12k 토큰. 초대형 파일 비용을 막는다
MAX_BYTES = 400_000  # corpus.py와 동일

MAXSIM = "maxsim"
MAXPOOL = "maxpool"
AGGREGATIONS = (MAXSIM, MAXPOOL)


@dataclass
class EncodeStats:
    n_texts: int = 0
    n_chunks: int = 0
    seconds: float = 0.0
    cache_hits: int = 0

    def __str__(self) -> str:
        return (f"texts={self.n_texts} chunks={self.n_chunks} "
                f"cache_hits={self.cache_hits} {self.seconds:.1f}s")


class ChunkedEncoder:
    """HF 모델로 청크 벡터를 만든다. `sentence-transformers` 없이 직접 풀링한다.

    이 환경의 torch는 1.13(CPU 전용)이라 최신 `sentence-transformers`가 맞지 않는다.
    풀링은 20줄이면 되므로 `transformers`만 쓴다.
    """

    def __init__(self, model_name: str = DEFAULT_MODEL, *,
                 max_len: int = MAX_LEN, stride: int = STRIDE,
                 max_chunks: int = MAX_CHUNKS, batch_size: int = 64,
                 threads: int | None = None):
        import torch
        from transformers import AutoModel, AutoTokenizer

        self.torch = torch
        if threads:
            torch.set_num_threads(threads)
        self.model_name = model_name
        self.tok = AutoTokenizer.from_pretrained(model_name)
        self.model = AutoModel.from_pretrained(model_name)
        self.model.eval()
        self.dim = int(self.model.config.hidden_size)

        self.max_len = max_len
        self.stride = stride
        self.max_chunks = max_chunks
        self.batch_size = batch_size
        self.stats = EncodeStats()

        self.cls_id = self.tok.cls_token_id
        self.sep_id = self.tok.sep_token_id
        self.pad_id = self.tok.pad_token_id or 0

    # ── 청크 분할 ────────────────────────────────────────────────────────
    def chunk_ids(self, text: str) -> list[list[int]]:
        """텍스트를 토큰 윈도로 자른다. 특수토큰은 윈도마다 붙인다."""
        ids = self.tok(text, add_special_tokens=False,
                       truncation=False)["input_ids"]
        if not ids:
            return []
        body = self.max_len - 2
        out = []
        for start in range(0, len(ids), self.stride):
            window = ids[start:start + body]
            if not window:
                break
            out.append([self.cls_id] + window + [self.sep_id])
            if len(out) >= self.max_chunks:
                break
            if start + body >= len(ids):
                break
        return out

    # ── 인코딩 ───────────────────────────────────────────────────────────
    def _encode_batches(self, chunks: list[list[int]]) -> np.ndarray:
        """청크 토큰열 → L2 정규화된 mean-pool 벡터 (n_chunks, dim)."""
        torch = self.torch
        vecs = np.zeros((len(chunks), self.dim), dtype=np.float32)
        t0 = time.time()
        with torch.no_grad():
            for i in range(0, len(chunks), self.batch_size):
                batch = chunks[i:i + self.batch_size]
                width = max(len(c) for c in batch)
                ids = torch.full((len(batch), width), self.pad_id, dtype=torch.long)
                mask = torch.zeros((len(batch), width), dtype=torch.long)
                for j, c in enumerate(batch):
                    ids[j, :len(c)] = torch.tensor(c, dtype=torch.long)
                    mask[j, :len(c)] = 1
                hidden = self.model(input_ids=ids, attention_mask=mask).last_hidden_state
                m = mask.unsqueeze(-1).float()
                # 패딩을 평균에서 제외한다. 안 빼면 짧은 청크가 0쪽으로 끌려간다.
                pooled = (hidden * m).sum(1) / m.sum(1).clamp(min=1e-9)
                pooled = torch.nn.functional.normalize(pooled, p=2, dim=1)
                vecs[i:i + len(batch)] = pooled.cpu().numpy()
        self.stats.n_chunks += len(chunks)
        self.stats.seconds += time.time() - t0
        return vecs

    def encode_text(self, text: str) -> np.ndarray:
        """텍스트 하나 → (n_chunks, dim). 빈 텍스트면 (0, dim)."""
        self.stats.n_texts += 1
        chunks = self.chunk_ids(text)
        if not chunks:
            return np.zeros((0, self.dim), dtype=np.float32)
        return self._encode_batches(chunks)

    def encode_query(self, text: str) -> np.ndarray:
        """쿼리 → 단일 벡터.

        쿼리는 하나의 일관된 문서이므로 청크 평균 후 재정규화한다.
        파일과 달리 "어딘가 한 군데만 맞으면 된다"가 아니라 전체가 질의다.
        """
        v = self.encode_text(text)
        if len(v) == 0:
            return np.zeros(self.dim, dtype=np.float32)
        m = v.mean(axis=0)
        n = np.linalg.norm(m)
        return (m / n).astype(np.float32) if n > 0 else m.astype(np.float32)


# ── 집계 ──────────────────────────────────────────────────────────────────

def score_file(query_vec: np.ndarray, chunk_vecs: np.ndarray,
               aggregation: str = MAXSIM) -> float:
    """쿼리 벡터와 파일 청크 벡터들의 유사도.

    반환값은 **[0, 1]로 클립된 코사인**이다. 음수 유사도는 0으로 만든다 —
    이 점수가 PPR teleport 가중치로 쓰이는데 확률분포에 음수를 넣을 수 없기
    때문이다(`teleport.build_teleport`가 음수를 거부한다).
    """
    if len(chunk_vecs) == 0 or not query_vec.any():
        return 0.0
    if aggregation == MAXSIM:
        return float(np.clip((chunk_vecs @ query_vec).max(), 0.0, 1.0))
    if aggregation == MAXPOOL:
        v = chunk_vecs.max(axis=0)
        n = np.linalg.norm(v)
        if n == 0:
            return 0.0
        return float(np.clip(float(v @ query_vec) / n, 0.0, 1.0))
    raise ValueError(f"unknown aggregation: {aggregation!r} (expected {AGGREGATIONS})")


# ── blob 캐시 ─────────────────────────────────────────────────────────────

@dataclass
class BlobCache:
    """blob SHA → 청크 벡터. npz로 디스크에 남긴다."""

    path: Path | None = None
    vecs: dict[str, np.ndarray] = field(default_factory=dict)

    @classmethod
    def load(cls, path: Path | None) -> "BlobCache":
        if path and path.exists():
            z = np.load(path)
            return cls(path=path, vecs={k: z[k] for k in z.files})
        return cls(path=path)

    #: 이만큼 새 blob이 쌓이면 자동 저장. 0이면 자동 저장 안 함.
    autosave_every: int = 200
    _since_save: int = 0

    def save(self) -> None:
        """원자적으로 쓴다. 쓰는 도중 죽어도 기존 캐시가 살아남는다."""
        if not self.path:
            return
        self.path.parent.mkdir(parents=True, exist_ok=True)
        # np.savez_compressed 는 확장자가 .npz 가 아니면 .npz 를 덧붙인다.
        # 그래서 tmp 이름도 반드시 .npz 로 끝나야 rename 대상이 일치한다.
        tmp = self.path.with_suffix(".tmp.npz")
        np.savez_compressed(tmp, **self.vecs)
        tmp.replace(self.path)          # POSIX 원자적 rename
        self._since_save = 0

    def get_or_encode(self, sha: str, text: str, enc: ChunkedEncoder) -> np.ndarray:
        v = self.vecs.get(sha)
        if v is not None:
            enc.stats.cache_hits += 1
            return v
        v = enc.encode_text(text)
        self.vecs[sha] = v
        # 중단돼도 여기까지 계산한 건 남는다. 이게 없으면 프로세스가 죽는 순간
        # 전부 사라진다 (실제로 35분어치를 잃은 적이 있다).
        self._since_save += 1
        if self.autosave_every and self._since_save >= self.autosave_every:
            self.save()
        return v
