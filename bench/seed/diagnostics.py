"""BM25 seed 진단 리포트.

**여기서 나오는 수치는 랭킹 지표가 아니다.** 공식 Recall/MRR/NDCG는
`bench/metrics/ranking.py`(담당 `bench-harness`)를 호출하는 `bench.seed.evaluate`가 낸다.
이 모듈은 그 지표로는 보이지 않는 것 — 누출률, corpus 도달성, 동점 덩어리,
teleport 계약 위반 — 을 보기 위한 **원자료 요약**이다.

  - 누출 진단: gold 경로가 쿼리에 그대로 남아 있는 인스턴스 비율 (조건별)
  - corpus 도달성: gold 파일이 corpus 안에 있는지 (Recall 상한의 원자료)
  - `bm25_rank` 컬럼의 gold 행 분포 — 이건 산출 피처를 그대로 읽은 것이지
    지표 구현이 아니다. 동점은 라벨 없이 **최악 순위**로 처리돼 있어
    공식 지표보다 낙관적일 수 없다 (다중 gold 동점 그룹에서 오히려 약간 비관적).
"""

from __future__ import annotations

import sys
from pathlib import Path

import pandas as pd

from bench.schema import C, DATA_DIR
from bench.seed import textproc
from bench.seed.dataset import load_instances
from bench.seed.teleport import build_teleport, validate_teleport

KS = (1, 3, 5, 10, 20, 50)


def gold_rank_table(long_df: pd.DataFrame, gold: dict[str, set[str]]) -> pd.DataFrame:
    """gold 행의 `bm25_rank`만 뽑는다 (지표 아님 — 피처 컬럼의 부분집합)."""
    g = pd.DataFrame(
        [(iid, f) for iid, fs in gold.items() for f in fs],
        columns=[C.instance_id, C.file_id],
    )
    return long_df.merge(g, on=[C.instance_id, C.file_id], how="inner")


def summarize(long_df: pd.DataFrame, leak_df: pd.DataFrame,
              gold: dict[str, set[str]]) -> str:
    lines: list[str] = []
    add = lines.append

    add("=" * 78)
    add("BM25 seed 진단 — 원자료 요약 (공식 랭킹 지표 아님)")
    add("=" * 78)

    n_inst = long_df[C.instance_id].nunique()
    add(f"instances={n_inst}  rows={len(long_df)}  "
        f"docs/instance={long_df.groupby([C.instance_id, 'condition']).size().mean():.0f}")

    # ── 1. corpus 도달성 ──────────────────────────────────────────────────
    add("")
    add("[1] corpus 도달성 (Recall 상한의 원자료)")
    per = leak_df[leak_df["condition"] == textproc.FULL]
    tot_gold = int(per["gold_n"].sum())
    tot_in = int(per["gold_in_corpus"].sum())
    add(f"    gold 파일 {tot_gold}개 중 corpus에 존재: {tot_in} ({tot_in / tot_gold:.1%})")
    full_cov = (per["gold_in_corpus"] == per["gold_n"]).mean()
    add(f"    gold 전부가 corpus에 있는 인스턴스: {full_cov:.1%}")

    # ── 2. 경로 누출 ─────────────────────────────────────────────────────
    add("")
    add("[2] gold 경로 누출률 — 쿼리에 정답 경로가 그대로 들어 있는 인스턴스 비율")
    add(f"    {'condition':<10} {'전체경로 일치':>14} {'basename 일치':>14}")
    for cond in textproc.CONDITIONS:
        s = leak_df[leak_df["condition"] == cond]
        add(f"    {cond:<10} {s['any_full_path'].mean():>13.1%} {s['any_basename'].mean():>13.1%}")

    # ── 3. gold의 bm25_rank 분포 ─────────────────────────────────────────
    add("")
    add("[3] gold 파일의 bm25_rank 분포 (동점=최악순위, 라벨 미사용)")
    add("    ※ 공식 Recall@k 아님 — 공식 수치는 `python -m bench.seed.evaluate`.")
    gr = gold_rank_table(long_df, gold)
    hdr = "    {:<10} {:>5}".format("condition", "n") + "".join(f"{'<='+str(k):>8}" for k in KS) + f"{'median':>9}"
    add(hdr)
    for cond in textproc.CONDITIONS:
        s = gr[gr["condition"] == cond]
        if s.empty:
            continue
        row = "    {:<10} {:>5}".format(cond, len(s))
        row += "".join(f"{(s[C.bm25_rank] <= k).mean():>8.1%}" for k in KS)
        row += f"{s[C.bm25_rank].median():>9.0f}"
        add(row)

    # ── 4. 점수 0 동점 덩어리 ────────────────────────────────────────────
    add("")
    add("[4] BM25=0 동점 덩어리 (PPR teleport / LTR 양쪽에 영향)")
    for cond in textproc.CONDITIONS:
        s = long_df[long_df["condition"] == cond]
        add(f"    {cond:<10} 전체 행 중 bm25==0: {(s[C.bm25] == 0).mean():>6.1%}"
            f"   gold 행 중 bm25==0: {(gr[gr['condition'] == cond][C.bm25] == 0).mean():>6.1%}")

    return "\n".join(lines)


def teleport_check(long_df: pd.DataFrame, condition: str,
                   graph_file_ids: dict[str, set[str]] | None = None) -> str:
    """실제 seed로 PPR teleport 계약을 강제 검증한다."""
    lines = ["", "[5] PPR teleport 벡터 계약 검증 (algo-core 인터페이스)"]
    sub = long_df[long_df["condition"] == condition]
    n_ok = 0
    fallbacks = 0
    dropped = []
    failures = []
    for iid, g in sub.groupby(C.instance_id):
        scored = list(zip(g[C.file_id], g[C.bm25]))
        gfids = graph_file_ids.get(iid) if graph_file_ids else None
        try:
            tv = build_teleport(scored, gfids)
            validate_teleport(tv, gfids)
            n_ok += 1
            fallbacks += int(tv.fallback_uniform)
            dropped.append(tv.dropped_mass)
        except Exception as e:
            failures.append((iid, f"{type(e).__name__}: {e}"))
    lines.append(f"    condition={condition}  통과 {n_ok}/{sub[C.instance_id].nunique()}"
                 f"  실패 {len(failures)}")
    lines.append(f"    균등 폴백(BM25 전부 0): {fallbacks}")
    if dropped:
        lines.append(f"    그래프 밖으로 버려진 평균 질량: {sum(dropped) / len(dropped):.2%}"
                     + ("  (그래프 노드 목록 미제공 — 실제 매트릭스 도착 후 재검증 필요)"
                        if graph_file_ids is None else ""))
    for iid, why in failures[:5]:
        lines.append(f"    FAIL {iid}: {why}")
    return "\n".join(lines)


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description="BM25 seed 진단")
    ap.add_argument("--data-dir", type=Path, default=DATA_DIR)
    ap.add_argument("--repo", default="pydata/xarray")
    ap.add_argument("--features-csv", type=Path, default=None,
                    help="주면 그 file_id를 그래프 노드 집합으로 보고 teleport를 검증한다")
    a = ap.parse_args(argv)

    long_df = pd.read_csv(a.data_dir / "bm25_seed_long.csv")
    leak_df = pd.read_csv(a.data_dir / "bm25_leakage.csv")
    gold = {i.instance_id: set(i.gold_files)
            for i in load_instances(repo=a.repo)}

    graph = None
    if a.features_csv:
        f = pd.read_csv(a.features_csv)
        graph = {k: set(v) for k, v in
                 f.groupby(C.instance_id)[C.file_id].apply(set).items()}

    print(summarize(long_df, leak_df, gold))
    print(teleport_check(long_df, textproc.NO_TRACE, graph))


if __name__ == "__main__":
    main(sys.argv[1:])
