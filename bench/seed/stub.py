"""계약 스키마에 맞춘 **스텁 피처 매트릭스** 생성기.

`bench-harness`의 Phase 1이 끝나기 전에 배관(조인·컬럼·teleport 변환)을 실제로
돌려보기 위한 것이다. 값은 난수이므로 **어떤 수치도 여기서 나오면 안 된다.**
목적은 오직 '실제 매트릭스가 들어왔을 때 코드가 그대로 돈다'를 보이는 것.
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from bench.schema import ALL_COLUMNS, C, FEATURE_COLUMNS


def make_stub_features(n_instances: int = 5, n_files: int = 40,
                       n_gold: int = 2, seed: int = 0) -> pd.DataFrame:
    rng = np.random.default_rng(seed)
    rows = []
    for i in range(n_instances):
        iid = f"stub__repo-{1000 + i}"
        files = [f"src/pkg/mod_{j:03d}.py" for j in range(n_files)]
        gold = set(rng.choice(files, size=min(n_gold, n_files), replace=False))
        for f in files:
            r = {C.instance_id: iid, C.file_id: f,
                 C.is_positive: int(f in gold),
                 C.is_generated: 0, C.commit_skew: 0}
            for col in FEATURE_COLUMNS:
                r[col] = float(rng.random())
            r[C.ppr] = np.nan          # Phase 3까지 비어 있음 (계약 명시)
            r[C.bm25] = np.nan         # retrieval이 채운다
            r[C.bm25_rank] = np.nan
            r[C.file_rank] = int(rng.integers(1, n_files + 1))
            rows.append(r)
    return pd.DataFrame(rows)[ALL_COLUMNS]


def make_stub_seed(features: pd.DataFrame, seed: int = 1) -> pd.DataFrame:
    """스텁 매트릭스와 조인 가능한 스텁 seed."""
    from bench.seed.run_seed import rank_pessimistic

    rng = np.random.default_rng(seed)
    out = []
    for iid, g in features.groupby(C.instance_id):
        fids = g[C.file_id].tolist()
        # 절반은 0점으로 두어 **대량 동점** 상황을 재현한다 — 실제 BM25의 모습이다.
        scores = [float(rng.random() * 10) if rng.random() < 0.5 else 0.0
                  for _ in fids]
        ranks = rank_pessimistic(scores)
        for f, s, r in zip(fids, scores, ranks):
            out.append({C.instance_id: iid, C.file_id: f,
                        C.bm25: s, C.bm25_rank: r})
    return pd.DataFrame(out)
