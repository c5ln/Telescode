"""어휘 중첩 판정 테스트.

이 정의가 최종 결론의 근거가 되므로, 판정이 의도대로 작동하는지 고정한다.
"""
from bench.seed.tokenizer import tokenize
from bench.seed.vocab import (IdentifierIndex, file_identifier_tokens,
                              overlap_for_instance)

SESSION = '''
class SessionManager:
    def refresh_token(self):
        pass

def _rotate_keys():
    pass
'''

LOGIN = '''
class LoginView:
    def do_login(self):
        pass
'''


def test_extracts_path_class_function_names():
    toks, ok = file_identifier_tokens(SESSION, "core/session.py")
    assert ok
    assert {"core", "session", "sessionmanager", "manager",
            "refresh_token", "refresh", "token", "rotate", "keys"} <= toks


def test_module_level_names_only_in_variant():
    src = "DEFAULT_ENCODING = 'utf8'\n"
    assert "encoding" not in file_identifier_tokens(src, "a.py")[0]
    assert "encoding" in file_identifier_tokens(src, "a.py", include_module_names=True)[0]


def test_syntax_error_reports_failure_but_keeps_path_tokens():
    toks, ok = file_identifier_tokens("def (", "pkg/broken.py")
    assert ok is False
    assert "broken" in toks and "pkg" in toks


def test_uses_same_tokenizer_as_bm25():
    """판정 토큰화가 BM25 인덱싱과 다르면 '겹침'이 BM25 실력과 어긋난다."""
    toks, _ = file_identifier_tokens("def get_records(): pass", "a.py")
    assert set(tokenize("get_records")) <= toks


def test_distinctive_drops_ubiquitous_tokens():
    # 'test'가 4개 파일 전부에 있으면 df=100% → 신호 아님
    idx = IdentifierIndex("i1", ids={
        f"f{i}.py": {"test", f"unique{i}"} for i in range(4)})
    d = idx.distinctive(max_df_ratio=0.10)
    assert "test" not in d
    assert "unique0" in d


def test_non_overlap_when_issue_never_names_the_file():
    """팀리드의 예시: 'login' 이슈가 세션 클래스를 한 번도 지목하지 않는 경우."""
    idx = IdentifierIndex("i1", ids={
        "auth/login.py": file_identifier_tokens(LOGIN, "auth/login.py")[0],
        "core/session.py": file_identifier_tokens(SESSION, "core/session.py")[0],
    })
    q = set(tokenize("Login fails after the do_login redirect"))
    flags = overlap_for_instance(idx, q, min_tokens=1, max_df_ratio=1.0)
    assert flags["auth/login.py"][0] is True
    assert flags["core/session.py"][0] is False, "BM25가 원리상 못 찾는 구간"


def test_min_tokens_threshold_is_enforced():
    idx = IdentifierIndex("i1", ids={"a.py": {"alpha", "beta"}, "b.py": {"alpha"}})
    q = {"alpha", "beta"}
    one = overlap_for_instance(idx, q, min_tokens=1, max_df_ratio=1.0)
    two = overlap_for_instance(idx, q, min_tokens=2, max_df_ratio=1.0)
    assert one["a.py"][0] and one["b.py"][0]
    assert two["a.py"][0] and not two["b.py"][0]


def test_overlap_tokens_are_reported_for_audit():
    idx = IdentifierIndex("i1", ids={"a.py": {"alpha", "beta"}})
    _, n, toks = overlap_for_instance(idx, {"alpha"}, max_df_ratio=1.0)["a.py"]
    assert n == 1 and toks == ["alpha"]
