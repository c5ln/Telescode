"""코드 인지 어휘 피처 — 유료 API 없이 BM25가 흐리는 신호를 따로 뽑는다.

BM25는 파일 하나를 토큰 가방 하나로 본다. 그래서 다음이 뭉개진다.

  def_match      이슈에 나온 식별자가 그 파일에서 **정의**(`def`/`class`)되는가.
                 `Dataset`을 쓰기만 하는 파일 수백 개와 정의하는 파일 하나가 BM25에서는
                 tf 차이로만 갈린다.
  str_match      이슈 본문의 메시지가 그 파일의 **문자열 리터럴**과 겹치는가.
                 에러 메시지는 `raise ...("...")`를 가진 파일을 거의 직접 가리키는데,
                 토큰화하면 흔한 단어로 흩어진다.
  nbr_bm25_*     import/호출로 이어진 이웃 파일들의 BM25 최댓값·평균.
                 PPR은 사실상 seed top-1만 옮기는데(RESULTS §5) 이건 모든 파일에
                 이웃의 어휘 점수를 직접 준다.
  mod_match      def_match와 같되 **모듈 수준 대입 이름**(`OPTIONS = ...`, 상수).
                 어휘 비중첩 gold 85개 중 14개가 여기서만 이슈와 겹쳤다.
  sym_use        이슈가 부른 심볼 중 **저장소 어딘가에 정의가 있고**, 이 파일은 정의하지
                 않고 **코드에서 사용만** 하는 것의 Σ idf(사용 df). 비중첩 gold의 다수가
                 이슈 단어를 정의가 아닌 사용처에만 갖고 있었다.
  sym_prop       sym_use 중 이 파일이 그 심볼의 **정의 파일과 간선으로 직접 연결**된 것만.
                 "이슈 심볼의 정의에서 호출 그래프를 한 칸 따라간다" — 전역 중심성이나
                 BM25 상위가 아니라 이슈가 지목한 심볼을 출발점으로 삼는 구조 신호다.

## 무결성

- 쿼리는 `textproc.make_query(..., condition)` — BM25 seed와 **같은 조건**이다.
  `no_paths`에서 트레이스백 프레임은 지워지고 예외 요약 줄은 남는다(textproc 설계).
  str_match가 그 요약 줄에서 신호를 얻는 것은 누출이 아니다 — 사용자가 보는 증상이다.
- 코드는 `base_commit` blob에서만 읽는다 (§3-3). `corpus.list_source_blobs` 재사용.
- 라벨을 보지 않는다. 임계값(식별자 3자, 리터럴 조각 12자·2단어)은 **사전에 고정**했고
  test를 보고 고르지 않았다 (§3-4).
- IDF는 인스턴스 안 파일 집합에서 센다. 흔한 이름(`get`, `name`)이 점수를 지배하지 않게.
- `schema.py`는 공유 계약(수정 금지)이라 컬럼 상수를 여기 `CX`에 둔다.

    bench/.venv/bin/python -m bench.features.code_lexical \
        --features bench/data/features_all_with_bm25__no_paths.csv \
        --edges bench/data/file_edges_all.csv \
        --add-repo pydata/xarray=bench/repos/xarray \
        --add-repo pytest-dev/pytest=bench/repos/pytest \
        --condition no_paths \
        --out bench/data/code_lexical__no_paths.csv
"""

from __future__ import annotations

import argparse
import math
import re
import sys
import time
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

import pandas as pd

from bench.schema import C
from bench.seed import textproc
from bench.seed.corpus import MAX_BYTES, _batch_read, list_source_blobs
from bench.seed.dataset import load_instances
from bench.seed.tokenizer import STOPWORDS


class CX:
    """이 모듈이 내는 컬럼명. `bench.schema.C`와 같은 역할."""

    def_match = "def_match"
    str_match = "str_match"
    nbr_bm25_max = "nbr_bm25_max"
    nbr_bm25_mean = "nbr_bm25_mean"
    mod_match = "mod_match"
    sym_use = "sym_use"
    sym_prop = "sym_prop"


# ablation 단위. `ltr.run_ltr`가 그룹별로 넣고 뺀다.
EXTRA_GROUPS: dict[str, list[str]] = {
    "def_match": [CX.def_match],
    "str_match": [CX.str_match],
    "nbr_bm25": [CX.nbr_bm25_max, CX.nbr_bm25_mean],
    "mod_match": [CX.mod_match],
    "sym": [CX.sym_use, CX.sym_prop],
}
EXTRA_COLUMNS = [c for cols in EXTRA_GROUPS.values() for c in cols]

MIN_IDENT_LEN = 3
MIN_SEGMENT_CHARS = 12
MIN_SEGMENT_WORDS = 2
MIN_COVERAGE = 0.99

_DEF = re.compile(r"^[ \t]*(?:async[ \t]+)?(?:def|class)[ \t]+([A-Za-z_]\w*)", re.M)
_IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_TRIPLE = re.compile(r'(?s)("""|\'\'\').*?\1')
_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"|\'((?:[^\'\\\n]|\\.)*)\'')
# str.format / f-string 치환부와 %-포맷 지정자. 리터럴을 이 자리에서 끊어 고정 조각만 남긴다.
_PLACEHOLDER = re.compile(
    r"\{[^{}]*\}"
    r"|%\([^)]*\)[#0 +-]*\d*(?:\.\d+)?[a-zA-Z]"
    r"|%[#0 +-]*\d*(?:\.\d+)?[sdrifgeExXco]")
_ALPHA_WORD = re.compile(r"[a-z]{2,}")
_WS = re.compile(r"\s+")
# 들여쓰기 없는 대입 `NAME = ...` / `NAME: T = ...`. `==` 비교는 제외.
_MODNAME = re.compile(r"^([A-Za-z_]\w*)[ \t]*(?::[^=\n]*)?=(?!=)", re.M)
_COMMENT = re.compile(r"#[^\n]*")


def normalize_text(text: str) -> str:
    return _WS.sub(" ", text.lower()).strip()


def defined_names(source: str) -> frozenset[str]:
    """파일이 정의하는 함수·클래스 이름 (소문자). 매직 메서드는 뺀다."""
    return frozenset(
        n.lower() for n in _DEF.findall(source)
        if len(n) >= MIN_IDENT_LEN and not (n.startswith("__") and n.endswith("__")))


def module_names(source: str) -> frozenset[str]:
    """모듈 수준 대입 이름 (소문자). `__all__` 같은 dunder는 뺀다."""
    return frozenset(
        n.lower() for n in _MODNAME.findall(source)
        if len(n) >= MIN_IDENT_LEN and not (n.startswith("__") and n.endswith("__")))


def used_identifiers(source: str) -> frozenset[str]:
    """코드에 등장하는 식별자 (소문자). docstring·주석은 지운다 — 설명문 속 단어는 사용이 아니다.

    정의 이름도 포함된다. "사용만 하는" 판정은 호출부가 정의 집합을 빼서 한다.
    """
    body = _COMMENT.sub(" ", _TRIPLE.sub(" ", source))
    return frozenset(w.lower() for w in _IDENT.findall(body) if len(w) >= MIN_IDENT_LEN)


def query_identifiers(query: str) -> frozenset[str]:
    """쿼리의 식별자 후보 (소문자). 불용어는 `tokenizer.STOPWORDS`와 같은 목록."""
    return frozenset(
        w.lower() for w in _IDENT.findall(query)
        if len(w) >= MIN_IDENT_LEN and w.lower() not in STOPWORDS)


def literal_segments(source: str) -> frozenset[str]:
    """한 줄짜리 문자열 리터럴의 고정 조각들 (정규화됨).

    docstring(삼중 따옴표)은 먼저 지운다 — 설명문이 이슈 산문과 겹치는 것은
    "이 파일이 그 메시지를 낸다"는 신호가 아니다.
    """
    body = _TRIPLE.sub(" ", source)
    out = set()
    for m in _LITERAL.finditer(body):
        lit = m.group(1) if m.group(1) is not None else m.group(2)
        lit = lit.replace("\\n", " ").replace("\\t", " ")
        for piece in _PLACEHOLDER.split(lit):
            seg = normalize_text(piece)
            if (len(seg) >= MIN_SEGMENT_CHARS
                    and len(_ALPHA_WORD.findall(seg)) >= MIN_SEGMENT_WORDS):
                out.add(seg)
    return frozenset(out)


def _idf(df: int, n: int) -> float:
    """`seed.bm25.BM25.idf`와 같은 평활화형."""
    return math.log(1.0 + (n - df + 0.5) / (df + 0.5))


def _idf_sum_scores(per_file: Sequence[frozenset[str]], hits: set[str]) -> list[float]:
    # 합산 순서를 정렬로 고정한다. set 순회 순서는 문자열 해시 무작위화로 실행마다
    # 달라지고, 부동소수점 덧셈 순서가 바뀌면 1e-14 수준 차이가 동점 순서를 뒤집는다.
    df = Counter(x for items in per_file for x in items if x in hits)
    n = len(per_file)
    return [sum(_idf(df[x], n) for x in sorted(items) if x in hits) for items in per_file]


def def_match_scores(defs: Sequence[frozenset[str]], query_ids: frozenset[str]) -> list[float]:
    """파일별 Σ idf(이름) — 쿼리 식별자 중 그 파일이 정의하는 것만."""
    return _idf_sum_scores(defs, set(query_ids))


def str_match_scores(segs: Sequence[frozenset[str]], query_norm: str) -> list[float]:
    """파일별 Σ idf(조각) — 정규화된 쿼리에 부분문자열로 등장하는 조각만."""
    universe = {s for items in segs for s in items}
    hits = {s for s in universe if s in query_norm}
    return _idf_sum_scores(segs, hits)


def symbol_scores(defs: Sequence[frozenset[str]], used: Sequence[frozenset[str]],
                  file_ids: Sequence[str], query_ids: frozenset[str],
                  edges: Iterable[tuple[str, str]]) -> tuple[list[float], list[float]]:
    """(sym_use, sym_prop) — 파일 순서는 `file_ids`와 같다.

    `defs[i]`는 i번 파일의 정의 이름(함수·클래스·모듈 대입). 이슈 식별자 중 정의가
    **하나라도 있는** 것만 심볼로 본다 — 정의 없는 단어는 저장소의 개념이 아니다.
    idf는 **사용 df**로 센다. 수백 파일이 부르는 심볼은 가리키는 힘이 약하다.
    """
    definers: dict[str, set[str]] = defaultdict(set)
    for fid, d in zip(file_ids, defs):
        for s in d & query_ids:
            definers[s].add(fid)
    symbols = set(definers)

    only_used = [(u & symbols) - d for u, d in zip(used, defs)]
    df = Counter(s for items in only_used for s in items)
    n = len(file_ids)

    nbrs: dict[str, set[str]] = defaultdict(set)
    for s, t in edges:
        nbrs[s].add(t)
        nbrs[t].add(s)

    use, prop = [], []
    for fid, items in zip(file_ids, only_used):
        ordered = sorted(items)       # 합산 순서 고정 (`_idf_sum_scores` 주석)
        use.append(sum(_idf(df[s], n) for s in ordered))
        near = nbrs.get(fid, set())
        prop.append(sum(_idf(df[s], n) for s in ordered if definers[s] & near))
    return use, prop


def neighbor_aggregates(file_ids: Iterable[str], bm25: dict[str, float],
                        edges: Iterable[tuple[str, str]]) -> tuple[dict, dict]:
    """무방향 이웃의 BM25 (최댓값, 평균). 이웃이 없으면 0.0 — 결측이 아니라 실제 0이다."""
    nbrs: dict[str, set[str]] = defaultdict(set)
    for s, t in edges:
        nbrs[s].add(t)
        nbrs[t].add(s)
    mx, mean = {}, {}
    for f in file_ids:
        vals = [bm25[n] for n in sorted(nbrs.get(f, ())) if n in bm25]
        mx[f] = max(vals) if vals else 0.0
        mean[f] = sum(vals) / len(vals) if vals else 0.0
    return mx, mean


@dataclass(frozen=True)
class BlobInfo:
    defs: frozenset[str]        # def/class 이름
    segs: frozenset[str]        # 문자열 리터럴 조각
    mods: frozenset[str]        # 모듈 수준 대입 이름
    used: frozenset[str]        # 코드 속 식별자


# blob sha → BlobInfo. 인스턴스 간 파일 대부분이 같다.
_BLOB_CACHE: dict[str, BlobInfo] = {}


def _blob_features(repo_dir: Path, commit: str, file_ids: set[str]) -> dict[str, BlobInfo]:
    blobs = [(p, s) for p, s in list_source_blobs(repo_dir, commit) if p in file_ids]
    need = sorted({s for _, s in blobs if s not in _BLOB_CACHE})
    for sha, raw in _batch_read(repo_dir, need).items():
        src = raw[:MAX_BYTES].decode("utf-8", errors="replace")
        _BLOB_CACHE[sha] = BlobInfo(defined_names(src), literal_segments(src),
                                    module_names(src), used_identifiers(src))
    return {p: _BLOB_CACHE[s] for p, s in blobs if s in _BLOB_CACHE}


def build(features: pd.DataFrame, edges: pd.DataFrame, repos: dict[str, Path],
          condition: str) -> tuple[pd.DataFrame, list[str]]:
    from bench.features.edges import repo_of

    if features[C.bm25].isna().any():
        raise SystemExit("매트릭스의 bm25에 결측이 있다 — nbr_bm25를 만들 수 없다")

    ids = sorted(features[C.instance_id].unique())
    insts = {i.instance_id: i for i in load_instances(instance_ids=ids)}
    missing = sorted(set(ids) - set(insts))
    if missing:
        raise SystemExit(f"SWE-bench에 없는 인스턴스 {len(missing)}개: {missing[:5]}")

    edge_by = {k: list(zip(g["src"], g["tgt"])) for k, g in edges.groupby(C.instance_id)}
    no_edges = sorted(set(ids) - set(edge_by))
    if no_edges:
        raise SystemExit(f"간선이 없는 인스턴스 {len(no_edges)}개: {no_edges[:5]} "
                         "— edges.py 실패 인스턴스를 먼저 해결하라")

    out, notes = [], []
    n_rows, n_covered = 0, 0
    t0 = time.monotonic()
    for n, (iid, g) in enumerate(features.groupby(C.instance_id, sort=True), 1):
        inst = insts[iid]
        repo = repo_of(iid)
        if repo not in repos:
            raise SystemExit(f"--add-repo에 {repo}가 없다")
        fids = list(g[C.file_id])
        blob = _blob_features(repos[repo], inst.base_commit, set(fids))

        covered = [f for f in fids if f in blob]
        q = textproc.make_query(inst.problem_statement, condition)
        qids = query_identifiers(q)
        infos = [blob[f] for f in covered]
        dm = dict(zip(covered, def_match_scores([b.defs for b in infos], qids)))
        sm = dict(zip(covered, str_match_scores([b.segs for b in infos], normalize_text(q))))
        mm = dict(zip(covered, def_match_scores([b.mods for b in infos], qids)))
        su, sp = symbol_scores([b.defs | b.mods for b in infos], [b.used for b in infos],
                               covered, qids, edge_by[iid])
        su, sp = dict(zip(covered, su)), dict(zip(covered, sp))
        mx, mean = neighbor_aggregates(fids, dict(zip(g[C.file_id], g[C.bm25])),
                                       edge_by[iid])
        nan = float("nan")
        for f in fids:
            out.append({C.instance_id: iid, C.file_id: f,
                        CX.def_match: dm.get(f, nan),
                        CX.str_match: sm.get(f, nan),
                        CX.nbr_bm25_max: mx[f], CX.nbr_bm25_mean: mean[f],
                        CX.mod_match: mm.get(f, nan),
                        CX.sym_use: su.get(f, nan),
                        CX.sym_prop: sp.get(f, nan)})
        n_rows += len(fids)
        n_covered += len(covered)
        if n % 20 == 0 or n == len(ids):
            print(f"  [{n}/{len(ids)}] {time.monotonic() - t0:.0f}s", flush=True)

    ratio = n_covered / n_rows if n_rows else 0.0
    if ratio < MIN_COVERAGE:
        raise SystemExit(f"blob 커버리지 {ratio:.4f} < {MIN_COVERAGE} "
                         f"({n_covered}/{n_rows}) — file_id 정규화를 확인하라")
    notes.append(f"blob 커버리지 {n_covered}/{n_rows} ({ratio:.4f})")
    return pd.DataFrame(out), notes


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--features", required=True, help="bm25가 채워진 매트릭스")
    ap.add_argument("--edges", required=True, help="bench.features.edges 산출")
    ap.add_argument("--add-repo", action="append", default=[], metavar="REPO=DIR")
    ap.add_argument("--condition", default=textproc.NO_PATHS, choices=textproc.CONDITIONS,
                    help="쿼리 전처리 조건. 매트릭스 bm25와 같은 조건이어야 한다")
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)

    repos = {}
    for spec in args.add_repo:
        if "=" not in spec:
            raise SystemExit(f"--add-repo 형식은 REPO=DIR 이다: {spec!r}")
        r, d = spec.split("=", 1)
        repos[r] = Path(d).resolve()

    features = pd.read_csv(args.features,
                           usecols=[C.instance_id, C.file_id, C.is_positive, C.bm25])
    edges = pd.read_csv(args.edges)
    df, notes = build(features, edges, repos, args.condition)
    df.to_csv(args.out, index=False)
    for msg in notes:
        print(f"  - {msg}")

    # 진단 전용 — 라벨은 피처 계산에 쓰지 않았고 여기서 분포만 본다.
    d = df.merge(features[[C.instance_id, C.file_id, C.is_positive]],
                 on=[C.instance_id, C.file_id])
    for col in EXTRA_COLUMNS:
        pos = d[d[C.is_positive] == 1][col]
        neg = d[d[C.is_positive] == 0][col]
        print(f"  {col:14s} >0 비율  gold {(pos > 0).mean():.3f} · 비gold {(neg > 0).mean():.3f}")
    print(f"{len(df)}행 → {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
