"""개편 후 재측정(RESULTS.md §13) 산출물에 PPR seed 설정을 다시 남긴다. 아무것도 다시 돌리지 않는다.

재측정 당시 `pipeline.py`는 바이너리 md5만 manifest/provenance에 남겼다. 그래서
`--seed-csv`, `--seed-topk`, `--seed-power`는 산출물만 보고 알 수 없었다
(감사 후속 C). 지금은 `pipeline.seed_provenance`가 이 셋을 기록한다. 이 스크립트는
이미 만들어진 개편 후 재측정(RESULTS.md §13) 산출물에 같은 형식으로 덧붙인다.

인자를 적어 넣기만 하지 않고 **산출물로 확인한다.** train 인스턴스의 `ppr`이 train
스윕 CSV의 해당 config 값과 모든 행에서 같아야 한다. 같은 바이너리, 같은 seed, 같은
트리이므로 결정적이다. 다른 config(대조군)와는 달라야 한다. 둘 중 하나라도 어긋나면
아무것도 쓰지 않고 중단한다.

    bench/.venv/bin/python bench/scripts/resolver_fix/reemit_provenance.py
"""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

import pandas as pd

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from bench.collect.pipeline import seed_provenance  # noqa: E402
from bench.schema import C  # noqa: E402

D = Path("bench/data/resolver_fix")
SA = Path("bench/data/seed_all")

# (산출 접두, seed, weight col, topk, power, 확인용 스윕, 대조 config)
RUNS = [
    (D, "", SA / "bm25_seed__no_paths.csv", "bm25", 2, 1.0,
     D / "ppr_sweep_bm2514.csv", "topk1_pow1"),
    (D, "_embedppr", SA / "embed_seed__no_paths.csv", "embed", 2, 1.0,
     D / "ppr_sweep_embed14.csv", "topk1_pow1"),
    (D / "sens_topk1", "_embedppr", SA / "embed_seed__no_paths.csv", "embed", 1, 1.0,
     D / "ppr_sweep_embed14.csv", "topk2_pow1"),
]


def _cfg(topk: int, power: float) -> str:
    return f"topk{topk}_pow{power:g}"


def verify(features: pd.DataFrame, sweep: pd.DataFrame, cfg: str, other: str) -> dict:
    def mismatches(name: str) -> tuple[int, int]:
        s = sweep[sweep["config"] == name][[C.instance_id, C.file_id, C.ppr]]
        f = features[features[C.instance_id].isin(set(s[C.instance_id]))]
        m = f[[C.instance_id, C.file_id, C.ppr]].merge(
            s, on=[C.instance_id, C.file_id], how="left", suffixes=("", "_sweep"))
        m["ppr_sweep"] = m["ppr_sweep"].fillna(0.0)   # 파이프라인과 같은 0 채움
        return int((m[C.ppr] != m["ppr_sweep"]).sum()), len(m)

    bad, n = mismatches(cfg)
    bad_other, _ = mismatches(other)
    return {"config": cfg, "rows_checked": n, "mismatched_rows": bad,
            "control_config": other, "control_mismatched_rows": bad_other}


def main() -> int:
    import os
    os.chdir(ROOT)
    sweeps = {}
    plan = []
    for out_dir, suf, seed, col, topk, power, sweep_path, other in RUNS:
        sweep = sweeps.setdefault(sweep_path, pd.read_csv(sweep_path))
        seed_prov = seed_provenance(str(seed), col, topk, power)
        for name in ("xarray", "pytest"):
            feat = pd.read_csv(out_dir / f"features_{name}{suf}.csv")
            chk = verify(feat, sweep, _cfg(topk, power), other)
            print(f"{out_dir}/{name}{suf}: {chk}")
            if chk["mismatched_rows"] or not chk["control_mismatched_rows"]:
                print("  → 확인 실패. 아무것도 쓰지 않는다", file=sys.stderr)
                return 1
            plan.append((out_dir / f"instances_{name}{suf}.csv", seed_prov, chk))
        plan.append((out_dir / f"instances_all{suf}.csv", seed_prov, None))

    for manifest_path, seed_prov, chk in plan:
        man = pd.read_csv(manifest_path)
        for k, v in seed_prov.items():
            man[k] = v
        man.to_csv(manifest_path, index=False)
        prov_path = manifest_path.with_suffix(".provenance.json")
        if prov_path.exists():
            prov = json.loads(prov_path.read_text())
            prov["ppr_seed"] = seed_prov
            prov["ppr_seed_reemitted"] = {
                "by": "bench/scripts/resolver_fix/reemit_provenance.py",
                "note": "실행 후에 기록. 값은 train 스윕 CSV와 ppr을 대조해 확인했다",
                "verification": chk}
            prov_path.write_text(json.dumps(prov, indent=2, ensure_ascii=False))
        print(f"기록 → {manifest_path}")

    # 스윕 provenance (ppr_sweep.py는 이제 직접 쓴다)
    for sweep_path, seed, col in ((D / "ppr_sweep_bm2514.csv", SA / "bm25_seed__no_paths.csv", "bm25"),
                                  (D / "ppr_sweep_embed14.csv", SA / "embed_seed__no_paths.csv", "embed")):
        splits = Path("bench/data/splits_all.csv")
        prov = {"pin": "93f9cf2", "sweep_csv": str(sweep_path),
                "seed": {"seed_csv": str(seed),
                         "seed_md5": hashlib.md5(seed.read_bytes()).hexdigest(),
                         "seed_weight_col": col},
                "grid": sorted(sweeps[sweep_path]["config"].unique()),
                "splits_csv": str(splits),
                "splits_md5": hashlib.md5(splits.read_bytes()).hexdigest(),
                "n_instances": int(sweeps[sweep_path][C.instance_id].nunique()),
                "reemitted_by": "bench/scripts/resolver_fix/reemit_provenance.py"}
        out = sweep_path.with_suffix(".provenance.json")
        out.write_text(json.dumps(prov, indent=2, ensure_ascii=False))
        print(f"기록 → {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
