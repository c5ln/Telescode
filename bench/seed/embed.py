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

import hashlib
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
    #: 청크에 실제로 들어간 토큰 수. 겹침 구간은 **중복 계산된다** — API가
    #: 보낸 만큼 과금하므로 그게 곧 청구 기준이다.
    n_tokens: int = 0
    seconds: float = 0.0
    cache_hits: int = 0

    def __str__(self) -> str:
        return (f"texts={self.n_texts} chunks={self.n_chunks} "
                f"tokens={self.n_tokens:,} "
                f"cache_hits={self.cache_hits} {self.seconds:.1f}s")


class ChunkedEncoder:
    """HF 모델로 청크 벡터를 만든다. `sentence-transformers` 없이 직접 풀링한다.

    ⚠ **이 환경에서는 동작하지 않는다.** 두 가지가 겹쳤다.

      1. 성능: torch 1.13(Debian 패키지, MKL 없음) + Ryzen 5 5500U에서
         0.47 chunks/s. 전체 코퍼스에 약 5.3일이 걸린다 — 애초에 못 쓴다.
      2. 의존성: Qwen3 토크나이저를 읽으려면 `tokenizers>=0.21`이 필요해
         `transformers`를 5.x로 올렸는데, 5.x는 torch>=2.5를 요구한다.
         그래서 지금 이 클래스의 `AutoModel.from_pretrained`는 실패한다.

    2번은 1번 때문에 감수한 것이다. 되살리려면 torch 2.x를 설치해야 하는데,
    그러면 1번(5.3일)이 그대로 남는다. 실사용 경로는 `api_encoder.py`다.
    이 클래스는 참조 구현 겸 다른 머신용으로 남긴다.
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

class CacheFingerprintError(RuntimeError):
    """다른 인코더 설정으로 만든 캐시를 재사용하려 했다. 아래 §지문 참조."""


#: npz 안에서 지문을 담는 예약 키. blob SHA는 40자 hex라 절대 충돌하지 않는다.
FINGERPRINT_KEY = "__encoder_fingerprint__"


@dataclass
class BlobCache:
    """blob SHA → 청크 벡터. npz로 디스크에 남긴다.

    ## 지문 (중요)

    캐시 키는 blob SHA다. 그런데 **SHA는 "어떤 인코더가 만든 벡터인가"를 담지
    않는다.** 지문이 없으면 MiniLM으로 만든 캐시를 Qwen3 실행이 그대로 주워
    쓴다. 차원이 다르면 운 좋게 터지지만, 같으면 **조용히 섞인다** — 서로 다른
    모델의 벡터끼리 코사인을 재면 그 숫자는 무의미한데 예외도 안 난다.

    그래서 인코더 설정(모델·차원·청크 파라미터·지시문)을 해시해 함께 저장하고,
    다르면 로드를 **거부한다**. 조용한 재사용보다 시끄러운 실패가 낫다.

    같은 이유로 **자동 폴백을 두지 않는다.** API가 죽었을 때 로컬 모델로 넘어가면
    한 실행 안에서 두 모델의 벡터가 섞인다. 실패하면 실패한 채로 멈춘다.
    """

    path: Path | None = None
    vecs: dict[str, np.ndarray] = field(default_factory=dict)
    fingerprint: str | None = None

    @classmethod
    def load(cls, path: Path | None, *,
             fingerprint: str | None = None) -> "BlobCache":
        """캐시를 읽는다. 지문이 다르면 `CacheFingerprintError`.

        Args:
            fingerprint: 지금 쓰려는 인코더의 지문. `encoder_fingerprint()` 참조.
                None이면 검사를 건너뛴다 — **테스트 전용이다.** 실제 파이프라인은
                반드시 넘긴다.
        """
        if not (path and path.exists()):
            return cls(path=path, fingerprint=fingerprint)

        z = np.load(path, allow_pickle=False)
        stored = None
        if FINGERPRINT_KEY in z.files:
            stored = str(z[FINGERPRINT_KEY].item())

        if fingerprint is not None and stored != fingerprint:
            raise CacheFingerprintError(
                f"캐시 {path} 는 다른 인코더 설정으로 만들어졌다.\n"
                f"  저장된 지문: {stored}\n"
                f"  현재 지문:   {fingerprint}\n"
                "다른 모델의 벡터가 섞이면 코사인 값이 무의미해진다. "
                "캐시를 지우고 다시 만들거나(--cache 경로 변경), 원래 설정으로 돌아가라.")

        vecs = {k: z[k] for k in z.files if k != FINGERPRINT_KEY}
        return cls(path=path, vecs=vecs, fingerprint=stored or fingerprint)

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
        payload = dict(self.vecs)
        if self.fingerprint:
            # 지문을 같이 굽는다. 이게 없으면 다음 실행이 다른 모델로 이어써도
            # 알 수 없다 (클래스 docstring §지문 참조).
            payload[FINGERPRINT_KEY] = np.asarray(self.fingerprint)
        np.savez_compressed(tmp, **payload)
        tmp.replace(self.path)          # POSIX 원자적 rename
        self._since_save = 0

    def bump(self) -> None:
        """벡터 하나를 직접 `vecs`에 넣은 뒤 호출한다. 임계치에 닿으면 저장한다.

        `get_or_encode`를 안 거치는 경로(배치 인코딩 — `run_embed.encode_corpus`)가
        증분 저장을 못 받는 문제 때문에 분리했다. 예전에는 자동 저장 훅이
        `get_or_encode` 안에만 있었는데 실제 뜨거운 경로가 그걸 우회해서,
        **캐시 파일이 한 번도 안 생겼다.** 그래서 중단될 때마다 전부 날아갔다.
        """
        self._since_save += 1
        if self.autosave_every and self._since_save >= self.autosave_every:
            self.save()

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


# ── 인코더 지문 ───────────────────────────────────────────────────────────

def encoder_fingerprint(enc) -> str:
    """인코더 설정을 사람이 읽을 수 있는 지문 한 줄로 만든다.

    `BlobCache`가 이 값으로 "이 캐시를 이 인코더가 써도 되는가"를 판정한다.
    벡터 값을 바꾸는 것은 전부 들어가야 한다:

      - `model`      : 모델이 다르면 벡터 공간 자체가 다르다
      - `dimensions` : MRL 절단 폭. 1024와 4096은 다른 벡터다
      - 청크 파라미터 : 경계가 달라지면 같은 파일이 다른 청크 집합이 된다
      - `instruction`: 쿼리에만 붙지만, 바뀌면 점수 전체가 바뀌므로 남긴다

    반대로 배치 크기·스레드 수·타임아웃은 **들어가면 안 된다.** 벡터 값을
    바꾸지 않는데 지문에 넣으면 배치만 조정해도 캐시가 통째로 버려진다.
    """
    model = getattr(enc, "model", None) or getattr(enc, "model_name", "?")
    parts = [
        f"model={model}",
        f"dim={getattr(enc, 'dimensions', None) or getattr(enc, 'dim', '?')}",
        f"max_len={enc.max_len}",
        f"stride={enc.stride}",
        f"max_chunks={enc.max_chunks}",
    ]
    instr = getattr(enc, "instruction", None)
    if instr:
        parts.append("instr=" + hashlib.sha256(instr.encode()).hexdigest()[:12])
    return " ".join(parts)
