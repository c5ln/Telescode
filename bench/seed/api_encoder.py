"""OpenRouter 임베딩 백엔드.

`embed.py`의 `ChunkedEncoder`(로컬 transformers)와 **같은 인터페이스**를 낸다.
`run_embed.py`는 둘 중 무엇을 받아도 동작한다.

## 왜 API인가

로컬 경로는 이 머신에서 쓸 수 없다. 실측:

    torch 1.13.0a0 (Debian 패키지, MKL not found)
    AMD Ryzen 5 5500U (6코어/12스레드)
    → 청크 32개 forward에 68.5초 = 0.47 chunks/s
    → 215,776 청크 = 약 127시간 (5.3일)

배칭 버그가 아니라 BLAS가 없는 빌드라서 나는 값이다. MiniLM(22M 파라미터)이
정상 환경 대비 약 70배 느리다. 배칭을 고쳐도 자릿수가 안 맞는다.

## 모델

`qwen/qwen3-embedding-8b` — $0.01/1M, 32K 컨텍스트, MRL(32~4096 차원 절단),
instruction-aware.

instruction-aware가 이 실험의 핵심이다. 쿼리에 태스크 지시문을 붙일 수 있어서
**"이 이슈와 비슷한 텍스트"가 아니라 "이 이슈를 고치려면 고쳐야 할 파일"**을
직접 물어볼 수 있다. 그게 Issue-conditioned Ranking 그 자체다.
모델 카드 기준 지시문 생략 시 검색 성능 1~5% 하락.

## 비대칭 인코딩

쿼리에만 지시문을 붙이고 문서(코드 청크)에는 붙이지 않는다. Qwen3-Embedding의
표준 사용법이며, 문서에도 붙이면 모든 문서가 같은 접두사를 공유해 서로 가까워진다.
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
from bench.seed.embed import EncodeStats

DEFAULT_API_MODEL = "qwen/qwen3-embedding-8b"
DEFAULT_BASE_URL = "https://openrouter.ai/api/v1"

#: 청크 토큰 수. 로컬 MiniLM은 512 한계 때문에 254였다. 32K 모델에서는
#: 그 제약이 없으므로 키운다 — `MAX_CHUNKS` 절단 문제(6,308개 중 1,760개,
#: 28%가 잘려나갔다)가 청크 수 자체를 줄여서 사라진다.
API_MAX_LEN = 1024
API_STRIDE = 768          # 겹침 25% — 로컬 설정(192/254)과 같은 비율
API_MAX_CHUNKS = 512      # 사실상 해제. 40만 토큰짜리 파일에만 걸린다

#: MRL 절단 차원. 4096 fp32면 청크 5.3만개에 869MB인데, 이 머신 가용 램이
#: 4GB뿐이라 여유를 둔다. Qwen3-Embedding은 32~4096 임의 절단을 지원한다.
API_DIMENSIONS = 1024

#: 쿼리 지시문. 위 "모델" 절 참조.
DEFAULT_INSTRUCTION = (
    "Given a GitHub issue report, retrieve the source files "
    "that must be modified to resolve it."
)

#: 요청당 입력 개수. OpenRouter가 상한을 문서화하지 않아 보수적으로 잡는다.
API_BATCH = 64
#: 요청당 총 문자 수 상한. 청크가 커서 개수만으로는 페이로드가 널뛴다.
API_BATCH_CHARS = 600_000

#: 동시 요청 수. 실측(무료 모델)으로 요청 하나가 ~5.8초라 순차로 보내면
#: 전체 75,498청크에 1.9시간이 걸린다. 왕복 지연이 지배적이므로 동시성이
#: 그대로 처리량이 된다. 8은 429를 안 맞으면서 15분대로 떨어지는 지점이다.
API_CONCURRENCY = 8


class ApiEncodeError(RuntimeError):
    """재시도를 다 쓰고도 실패한 요청. 조용히 넘기지 않는다 (CONTRACT.md §3-7)."""


@dataclass
class _Retry:
    tries: int = 6
    base: float = 1.5      # 초. 지수 백오프의 밑
    cap: float = 60.0


class OpenRouterEncoder:
    """OpenRouter 임베딩 API 래퍼.

    `ChunkedEncoder`와 같은 표면(`chunk_texts` / `encode_query` / `stats` / `dim`)을
    내지만, 파일 단위 `encode_text`는 **일부러 제공하지 않는다.** 파일마다 요청을
    보내면 배치가 청크 2~10개로 쪼개져 API 왕복이 지배적이 된다(로컬 경로가
    느렸던 이유 중 하나이기도 하다). 대신 `embed_texts`로 여러 파일의 청크를
    한꺼번에 넘긴다 — 호출자는 `run_embed.encode_corpus`다.
    """

    def __init__(self, model: str = DEFAULT_API_MODEL, *,
                 tokenizer_name: str = "Qwen/Qwen3-Embedding-8B",
                 max_len: int = API_MAX_LEN, stride: int = API_STRIDE,
                 max_chunks: int = API_MAX_CHUNKS,
                 dimensions: int | None = API_DIMENSIONS,
                 instruction: str = DEFAULT_INSTRUCTION,
                 batch: int = API_BATCH, batch_chars: int = API_BATCH_CHARS,
                 concurrency: int = API_CONCURRENCY,
                 api_key: str | None = None,
                 base_url: str = DEFAULT_BASE_URL,
                 timeout: float = 60.0):
        self.model = model
        self.max_len = max_len
        self.stride = stride
        self.max_chunks = max_chunks
        self.dimensions = dimensions
        self.instruction = instruction
        self.batch = batch
        self.batch_chars = batch_chars
        self.concurrency = max(1, concurrency)
        self._lock = threading.Lock()   # stats·dim은 워커 스레드가 함께 건드린다
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.retry = _Retry()
        self.stats = EncodeStats()

        # 저장소 루트 .env → os.environ. 쉘에서 export한 값이 항상 이긴다.
        load_dotenv()
        # 키 검사는 **첫 요청 때** 한다. `--dry-run`(청크·비용만 세기)은 키 없이
        # 돌아야 한다 — 돈 쓸지 결정하기 전에 비용을 보는 게 순서라서다.
        self._api_key = api_key or os.environ.get("OPENROUTER_API_KEY") or None

        self.tok = self._load_tokenizer(tokenizer_name)
        # dim은 첫 응답에서 확정한다. dimensions를 넘기면 그 값이지만,
        # 서버가 무시할 수도 있으므로 응답을 믿는다 (스모크 테스트로 확인).
        self.dim: int | None = dimensions

    # ── 토크나이저 ────────────────────────────────────────────────────────
    @staticmethod
    def _load_tokenizer(name: str):
        """청크 경계를 세려면 **그 모델의** 토크나이저가 필요하다.

        가중치는 안 받고 `tokenizer.json`만 받는다(~11MB). MiniLM 토크나이저로
        세면 청크 길이가 어긋나 32K 한계를 넘거나 낭비된다.

        `transformers.AutoTokenizer`를 쓰지 않고 `tokenizers`로 직접 연다.
        AutoTokenizer는 `tokenizer_config.json`의 `tokenizer_class`를 자기 레지스트리에서
        찾는데, Qwen3-Embedding은 그 값이 `Qwen2Tokenizer`다 — Qwen3가 Qwen2에서
        토큰화 방식(BPE)을 바꾸지 않아 로더 클래스를 재사용하기 때문이다(어휘는
        Qwen3 자신의 것으로 151,665개). 결국 클래스 이름 하나 때문에 transformers
        버전에 묶이는데, 우리가 필요한 건 offset 뿐이라 레지스트리를 우회한다.
        """
        from huggingface_hub import hf_hub_download
        from tokenizers import Tokenizer
        return Tokenizer.from_file(hf_hub_download(name, "tokenizer.json"))

    def chunk_texts(self, text: str) -> list[str]:
        """텍스트를 토큰 윈도로 자르되 **원문 부분문자열**을 돌려준다.

        토큰 id를 decode해서 되돌리지 않는다 — decode 왕복은 공백·특수문자에서
        원문과 어긋나고, 그 어긋남이 그대로 API에 실려 나간다. offset_mapping으로
        문자 구간을 얻어 원문을 슬라이스하면 손실이 없다.
        """
        if not text:
            return []
        offs = self.tok.encode(text, add_special_tokens=False).offsets
        if not offs:
            return []

        out: list[str] = []
        for start in range(0, len(offs), self.stride):
            window = offs[start:start + self.max_len]
            if not window:
                break
            c0, c1 = window[0][0], window[-1][1]
            piece = text[c0:c1]
            if piece.strip():
                out.append(piece)
                # 과금 기준을 정확히 센다. bytes/token 휴리스틱은 코드에서
                # 크게 빗나가고, 그 오차로 지출을 결정할 수는 없다.
                with self._lock:
                    self.stats.n_tokens += len(window)
            if len(out) >= self.max_chunks:
                break
            if start + self.max_len >= len(offs):
                break
        return out

    # ── HTTP ─────────────────────────────────────────────────────────────
    @property
    def api_key(self) -> str:
        """첫 사용 시점에 검사한다. 없으면 무엇을 해야 하는지 알려주고 죽는다."""
        if not self._api_key:
            self._api_key = require(
                "OPENROUTER_API_KEY", hint="키 발급: https://openrouter.ai/keys")
        return self._api_key

    def _post(self, inputs: list[str]) -> np.ndarray:
        body: dict = {"model": self.model, "input": inputs}
        if self.dimensions:
            body["dimensions"] = self.dimensions
        payload = json.dumps(body).encode("utf-8")

        last: Exception | None = None
        for attempt in range(self.retry.tries):
            req = urllib.request.Request(
                f"{self.base_url}/embeddings", data=payload, method="POST",
                headers={"Authorization": f"Bearer {self.api_key}",
                         "Content-Type": "application/json"})
            try:
                with urllib.request.urlopen(req, timeout=self.timeout) as r:
                    doc = json.loads(r.read().decode("utf-8"))
                return self._vectors_from(doc, len(inputs))
            except urllib.error.HTTPError as e:
                detail = e.read().decode("utf-8", errors="replace")[:400]
                last = ApiEncodeError(f"HTTP {e.code}: {detail}")
                # 4xx는 재시도해도 같은 답이다. 429(레이트리밋)만 예외.
                if e.code != 429 and e.code < 500:
                    raise last from e
            except (urllib.error.URLError, TimeoutError, OSError,
                    json.JSONDecodeError) as e:
                last = e
            # **재시도를 반드시 찍는다.** 이게 없으면 멈춘 것과 재시도 중인 것을
            # 구분할 수 없다. 실제로 5,053 blob 지점에서 30분간 조용히 멈춘 적이
            # 있고, py-spy를 붙이기 전까지 원인을 볼 수 없었다.
            print(f"    retry {attempt + 1}/{self.retry.tries} "
                  f"(inputs={len(inputs)}): {type(last).__name__} {str(last)[:120]}",
                  file=sys.stderr, flush=True)
            time.sleep(min(self.retry.base ** attempt, self.retry.cap))
        raise ApiEncodeError(f"{self.retry.tries}회 재시도 실패: {last}")

    def _vectors_from(self, doc: dict, n_expected: int) -> np.ndarray:
        data = doc.get("data")
        if not isinstance(data, list) or len(data) != n_expected:
            raise ApiEncodeError(
                f"응답 개수 불일치: 기대 {n_expected}, 실제 "
                f"{len(data) if isinstance(data, list) else type(data)} / {str(doc)[:300]}")
        # OpenAI 호환 응답은 index를 준다. 순서를 신뢰하지 말고 index로 되돌린다.
        rows = sorted(data, key=lambda d: d.get("index", 0))
        vecs = np.asarray([r["embedding"] for r in rows], dtype=np.float32)

        with self._lock:
            if self.dim is None:
                self.dim = vecs.shape[1]
            elif vecs.shape[1] != self.dim:
                raise ApiEncodeError(
                    f"차원이 흔들린다: 기대 {self.dim}, 실제 {vecs.shape[1]}. "
                    "dimensions 파라미터가 무시됐거나 라우팅이 바뀌었다")

        # score_file은 단위벡터를 가정한다(코사인을 내적으로 계산). MRL 절단은
        # 정규화를 깨뜨리므로 여기서 반드시 다시 정규화한다.
        n = np.linalg.norm(vecs, axis=1, keepdims=True)
        return (vecs / np.clip(n, 1e-12, None)).astype(np.float32)

    # ── 공개 API ──────────────────────────────────────────────────────────
    def _batches(self, texts: list[str]):
        """개수와 총 문자 수 **둘 다**로 자른다.

        청크가 1024토큰이라 개수만으로 자르면 배치 하나가 수 MB가 될 수 있다.
        """
        cur: list[str] = []
        chars = 0
        for t in texts:
            if cur and (len(cur) >= self.batch or chars + len(t) > self.batch_chars):
                yield cur
                cur, chars = [], 0
            cur.append(t)
            chars += len(t)
        if cur:
            yield cur

    def embed_texts(self, texts: list[str]) -> np.ndarray:
        """청크 텍스트 목록 → (n, dim) 정규화 행렬. 여러 파일 것을 섞어도 된다."""
        if not texts:
            return np.zeros((0, self.dim or 0), dtype=np.float32)
        t0 = time.time()
        batches = list(self._batches(texts))

        self._done_in_call = 0
        n_batches = len(batches)

        def one(b):
            v = self._post(b)
            with self._lock:
                self._done_in_call += 1
                d = self._done_in_call
            if d % 5 == 0 or d == n_batches:
                print(f"    batch {d}/{n_batches} ({time.time() - t0:.0f}s)",
                      file=sys.stderr, flush=True)
            return v

        if self.concurrency == 1 or len(batches) == 1:
            parts = [one(b) for b in batches]
        else:
            # 순서를 지켜야 한다. 호출자(encode_corpus)가 반환 행을 span으로
            # 잘라 파일에 되돌리므로, 한 배치라도 자리가 바뀌면 **파일과 벡터가
            # 어긋난 채로 조용히 끝난다.** map은 입력 순서를 보존한다.
            with ThreadPoolExecutor(max_workers=self.concurrency) as pool:
                parts = list(pool.map(one, batches))

        with self._lock:
            self.stats.n_chunks += len(texts)
            self.stats.seconds += time.time() - t0
        return np.vstack(parts)

    def encode_query(self, text: str) -> np.ndarray:
        """이슈 텍스트 → 단일 쿼리 벡터. **지시문은 여기에만 붙는다.**

        쿼리가 32K를 넘으면 앞부분만 쓴다. 이슈 본문이 그만큼 긴 경우는 없지만,
        조용히 잘리는 대신 청크 분할 후 평균을 내 일관성을 유지한다.
        """
        self.stats.n_texts += 1
        q = f"Instruct: {self.instruction}\nQuery: {text}" if self.instruction else text
        pieces = self.chunk_texts(q) or [q[:20_000]]
        v = self.embed_texts(pieces)
        if len(v) == 0:
            return np.zeros(self.dim or API_DIMENSIONS, dtype=np.float32)
        m = v.mean(axis=0)
        n = float(np.linalg.norm(m))
        return (m / n).astype(np.float32) if n > 0 else m.astype(np.float32)
