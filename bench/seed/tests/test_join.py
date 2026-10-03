from bench.schema import C
from bench.seed.join_check import join_seed
from bench.seed.stub import make_stub_features, make_stub_seed


def test_stub_roundtrip_joins_fully():
    f = make_stub_features()
    s = make_stub_seed(f)
    merged, rep = join_seed(f, s)
    assert rep.n_matched == len(f)
    assert rep.coverage == 1.0
    assert rep.instances_zero_match == []
    assert rep.ok()
    assert merged[C.bm25].notna().all()


def test_denormalized_paths_still_join():
    f = make_stub_features()
    s = make_stub_seed(f)
    s[C.file_id] = "./" + s[C.file_id]      # 하네스와 다른 표기
    _, rep = join_seed(f, s)
    assert rep.coverage == 1.0, "normalize_file_id가 양쪽을 맞춰야 한다"


def test_zero_match_is_detected():
    f = make_stub_features()
    s = make_stub_seed(f)
    s[C.file_id] = s[C.file_id] + ".bak"    # 고의로 어긋나게
    _, rep = join_seed(f, s)
    assert rep.n_matched == 0
    assert not rep.ok()
    assert len(rep.instances_zero_match) > 0
