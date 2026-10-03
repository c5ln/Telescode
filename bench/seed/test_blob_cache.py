"""`BlobCache` 지문 가드 테스트.

이 테스트가 지키는 실패는 **예외를 안 던지는 종류**다. 다른 모델로 만든 캐시를
재사용해도 코드는 끝까지 돌고 CSV도 나온다. 숫자만 무의미해진다.
그래서 "거부하는지"를 명시적으로 검사한다.
"""

import numpy as np
import pytest

from bench.seed.embed import (FINGERPRINT_KEY, BlobCache, CacheFingerprintError,
                              encoder_fingerprint)


class _FakeEnc:
    """지문 계산에 필요한 속성만 가진 가짜 인코더."""

    def __init__(self, **kw):
        self.model = kw.get("model", "qwen/qwen3-embedding-8b")
        self.dimensions = kw.get("dimensions", 1024)
        self.max_len = kw.get("max_len", 1024)
        self.stride = kw.get("stride", 768)
        self.max_chunks = kw.get("max_chunks", 512)
        self.instruction = kw.get("instruction", "find the files to modify")
        # 지문에 **들어가면 안 되는** 것들
        self.batch = kw.get("batch", 64)
        self.timeout = kw.get("timeout", 180.0)


def test_roundtrip_preserves_vectors_and_fingerprint(tmp_path):
    fp = encoder_fingerprint(_FakeEnc())
    c = BlobCache(path=tmp_path / "c.npz", fingerprint=fp)
    c.vecs["a" * 40] = np.ones((3, 4), dtype=np.float32)
    c.save()

    back = BlobCache.load(tmp_path / "c.npz", fingerprint=fp)
    assert set(back.vecs) == {"a" * 40}
    np.testing.assert_array_equal(back.vecs["a" * 40], np.ones((3, 4), np.float32))
    assert back.fingerprint == fp
    # 예약 키가 벡터로 새어나오면 안 된다
    assert FINGERPRINT_KEY not in back.vecs


@pytest.mark.parametrize("changed", [
    {"model": "openai/text-embedding-3-small"},   # 다른 모델 = 다른 벡터공간
    {"dimensions": 4096},                          # MRL 절단 폭이 다르면 다른 벡터
    {"max_len": 512},                              # 청크 경계가 달라진다
    {"stride": 256},
    {"max_chunks": 64},
    {"instruction": "something else entirely"},    # 점수 전체가 바뀐다
])
def test_load_rejects_mismatched_fingerprint(tmp_path, changed):
    """벡터 값을 바꾸는 설정이 하나라도 다르면 로드를 거부해야 한다."""
    written = BlobCache(path=tmp_path / "c.npz",
                        fingerprint=encoder_fingerprint(_FakeEnc()))
    written.vecs["b" * 40] = np.zeros((2, 4), dtype=np.float32)
    written.save()

    other = encoder_fingerprint(_FakeEnc(**changed))
    with pytest.raises(CacheFingerprintError):
        BlobCache.load(tmp_path / "c.npz", fingerprint=other)


@pytest.mark.parametrize("harmless", [{"batch": 8}, {"timeout": 5.0}])
def test_load_accepts_settings_that_do_not_change_vectors(tmp_path, harmless):
    """배치·타임아웃은 벡터를 안 바꾼다. 이걸로 캐시를 버리면 안 된다."""
    fp = encoder_fingerprint(_FakeEnc())
    c = BlobCache(path=tmp_path / "c.npz", fingerprint=fp)
    c.vecs["c" * 40] = np.ones((1, 4), dtype=np.float32)
    c.save()

    assert encoder_fingerprint(_FakeEnc(**harmless)) == fp
    back = BlobCache.load(tmp_path / "c.npz", fingerprint=fp)
    assert set(back.vecs) == {"c" * 40}


def test_legacy_cache_without_fingerprint_is_rejected(tmp_path):
    """지문 도입 이전에 만들어진 캐시(=MiniLM 산출물일 수 있다)도 거부한다.

    지문이 없다는 것은 "어느 모델이 만들었는지 모른다"는 뜻이다.
    모르는 것은 안전한 쪽으로 판정한다.
    """
    p = tmp_path / "legacy.npz"
    np.savez_compressed(p, **{"d" * 40: np.ones((1, 4), dtype=np.float32)})

    with pytest.raises(CacheFingerprintError):
        BlobCache.load(p, fingerprint=encoder_fingerprint(_FakeEnc()))


def test_missing_file_is_not_an_error(tmp_path):
    """캐시가 아직 없는 첫 실행은 정상이다."""
    fp = encoder_fingerprint(_FakeEnc())
    c = BlobCache.load(tmp_path / "nope.npz", fingerprint=fp)
    assert c.vecs == {}
    assert c.fingerprint == fp


def test_save_is_atomic_on_crash(tmp_path):
    """쓰다 죽어도 기존 캐시가 살아있어야 한다.

    임시파일 이름이 `.npz`로 안 끝나면 `np.savez_compressed`가 `.npz`를 덧붙여
    rename 대상이 어긋난다(실제로 겪은 버그다). 그 회귀를 막는다.
    """
    fp = encoder_fingerprint(_FakeEnc())
    p = tmp_path / "c.npz"
    first = BlobCache(path=p, fingerprint=fp)
    first.vecs["e" * 40] = np.ones((1, 4), dtype=np.float32)
    first.save()
    assert p.exists()
    # 임시파일이 남아 있으면 안 된다
    assert not list(tmp_path.glob("*.tmp.npz"))

    second = BlobCache.load(p, fingerprint=fp)
    second.vecs["f" * 40] = np.ones((1, 4), dtype=np.float32)
    second.save()
    assert set(BlobCache.load(p, fingerprint=fp).vecs) == {"e" * 40, "f" * 40}
