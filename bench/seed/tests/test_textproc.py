from bench.seed import textproc as T

TB = '''Something broke.

Traceback (most recent call last):
  File "/home/u/proj/src/_pytest/logging.py", line 699, in caplog_records
    return self._item.stash[caplog_records_key]
  File "/home/u/proj/src/_pytest/stash.py", line 12, in __getitem__
    raise KeyError(key)
KeyError: 'call'

Please fix.
'''


def test_full_keeps_everything():
    assert "logging.py" in T.make_query(TB, T.FULL)


def test_no_trace_removes_frames_and_source_lines():
    q = T.make_query(TB, T.NO_TRACE)
    assert "logging.py" not in q
    assert "stash.py" not in q
    assert "caplog_records_key" not in q, "프레임 뒤 소스 인용 줄도 지워야 한다"
    assert "Something broke." in q
    assert "Please fix." in q
    assert "KeyError" in q, "예외 요약은 증상 설명이므로 남긴다"


def test_pytest_location_lines_removed():
    txt = "src/_pytest/python.py:345: in call_fixture\nSKIPPED [1] conftest.py:6: Skipping\nkeep me"
    q = T.make_query(txt, T.NO_TRACE)
    assert "python.py" not in q
    assert "conftest.py" not in q
    assert "keep me" in q


def test_github_permalink_removed():
    txt = ("see https://github.com/pytest-dev/pytest/blob/28e8c85/src/_pytest/logging.py#L699 "
           "for details")
    q = T.make_query(txt, T.NO_TRACE)
    assert "logging.py" not in q
    assert "for details" in q


def test_no_paths_masks_prose_mentions():
    txt = "The bug is in src/_pytest/logging.py and also in helpers.py"
    assert "logging.py" in T.make_query(txt, T.NO_TRACE), "산문 언급은 NO_TRACE가 지우지 않는다"
    q = T.make_query(txt, T.NO_PATHS)
    assert "logging.py" not in q and "helpers.py" not in q


def test_leakage_report_shape():
    rep = T.leakage_report(TB, {"src/_pytest/logging.py"})
    assert set(rep) == set(T.CONDITIONS)
    assert rep[T.FULL]["any_full_path"] is True
    assert rep[T.NO_TRACE]["any_full_path"] is False
    assert rep[T.NO_PATHS]["any_basename"] is False


def test_unknown_condition_raises():
    try:
        T.make_query("x", "nope")
    except ValueError:
        return
    raise AssertionError("must raise")
