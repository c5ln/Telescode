"""연속 update 검사 — 한 DB에 update를 여러 번 이어서 한 뒤 새로 스캔한 DB와 비교한다.

`update_equiv`는 시나리오마다 새 DB에서 update를 한 번 한다. 여기서는 같은 DB에
본문 편집 `--body`개와 구조 편집(`STRUCTURAL`)을 시드로 섞어 차례로 적용하고,
마지막에만 `algo` 후 새 스캔과 비교한다. 앞선 update가 남긴 의존성 기록(ref_dep/fact)이
틀렸다면 뒤의 update가 고쳐야 할 ref를 놓치므로, 한 번짜리 시나리오로는 안 보이는
오류가 여기서 드러난다.

본문 편집: 아직 있는 .py 파일 중 하나와 그 안의 함수 하나를 시드로 고르고, 본문 첫 문장
앞에 같은 파일의 모듈 수준 함수 호출(없으면 `len(())`) 한 줄을 넣는다.

    bench/.venv/bin/python -m bench.tools.update_repeat --bin <TelescodeHeadless> \\
        --work /tmp/ur --out bench/results/update_incremental/new/repeat.json [--seed 5] [--body 20]

종료 코드: 마지막 비교가 같으면 0, 다르면 1.
"""

from __future__ import annotations

import argparse
import ast
import json
import random
import shutil
import sys
from pathlib import Path

from bench.tools.update_equiv import (ROOT, _md5, _rm_db, _run, _write, apply_edit,
                                      compact, compare_dbs, copy_py_tree, scan, update_argv)

# xarray 구조 편집: (편집들, update 명령들)
STRUCTURAL = [
    ([{"op": "append", "path": "xarray/core/utils.py",
       "text": "\n\ndef stage5_repeat_helper(x):\n    return is_dict_like(x)\n"},
      {"op": "append", "path": "xarray/core/formatting.py",
       "text": "\n\ndef stage5_repeat_user():\n"
               "    from xarray.core.utils import stage5_repeat_helper\n"
               "    return stage5_repeat_helper({})\n"}],
     [{"cmd": "files", "paths": ["xarray/core/utils.py", "xarray/core/formatting.py"]}]),
    ([{"op": "remove_def", "path": "xarray/core/utils.py", "name": "drop_dims_from_indexers"}],
     [{"cmd": "file", "path": "xarray/core/utils.py"}]),
    ([{"op": "add_method", "path": "xarray/backends/common.py", "class": "AbstractDataStore",
       "text": "def get_attrs_stage5(self):\n    return self.get_attrs()\n"}],
     [{"cmd": "file", "path": "xarray/backends/common.py"}]),
    ([{"op": "remove", "path": "xarray/util/print_versions.py"}],
     [{"cmd": "delete", "file_id": "xarray/util/print_versions.py"}]),
    ([{"op": "move", "path": "xarray/core/formatting_html.py",
       "to": "xarray/core/formatting_html_stage5.py"}],
     [{"cmd": "rename", "old": "xarray/core/formatting_html.py",
       "new": "xarray/core/formatting_html_stage5.py"}]),
]


def body_edit(tree: Path, rng: random.Random, n: int) -> str:
    """시드로 고른 함수 본문 맨 앞에 호출 한 줄을 넣고 그 파일의 상대경로를 돌려준다."""
    files = sorted(p for p in tree.rglob("*.py"))
    while True:
        p = rng.choice(files)
        text = p.read_text(encoding="utf-8")
        try:
            mod = ast.parse(text)
        except SyntaxError:
            continue
        funcs = [f for f in ast.walk(mod) if isinstance(f, (ast.FunctionDef, ast.AsyncFunctionDef))
                 and f.body[0].lineno > f.lineno and f.body[0].col_offset > 0]
        if not funcs:
            continue
        f = rng.choice(funcs)
        callees = sorted(t.name for t in mod.body if isinstance(t, ast.FunctionDef))
        call = rng.choice(callees) if callees else "len"
        line = " " * f.body[0].col_offset + f"_stage5_{n} = {call}()  # stage5\n"
        lines = text.splitlines(keepends=True)
        lines.insert(f.body[0].lineno - 1, line)
        new = "".join(lines)
        try:
            ast.parse(new)
        except SyntaxError:
            continue
        _write(p, new)
        return str(p.relative_to(tree))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bin", required=True)
    ap.add_argument("--repo", default="bench/repos/xarray")
    ap.add_argument("--work", required=True)
    ap.add_argument("--out")
    ap.add_argument("--seed", type=int, default=5)
    ap.add_argument("--body", type=int, default=20)
    args = ap.parse_args(argv)

    binary = str(Path(args.bin).resolve())
    work = Path(args.work).resolve()
    tree, db_a, db_b = work / "tree", work / "a.db", work / "b.db"
    if tree.exists():
        shutil.rmtree(tree)
    tree.mkdir(parents=True)
    copy_py_tree((ROOT / args.repo).resolve(), tree)

    rng = random.Random(args.seed)
    plan = ["body"] * args.body + list(range(len(STRUCTURAL)))
    rng.shuffle(plan)

    steps = []
    try:
        scan(binary, tree, db_a)
        for i, item in enumerate(plan):
            if item == "body":
                path = body_edit(tree, rng, i)
                ops = [{"cmd": "file", "path": path}]
                desc = f"body {path}"
            else:
                edits, ops = STRUCTURAL[item]
                for e in edits:
                    apply_edit(tree, e)
                desc = f"structural {item}: " + ", ".join(
                    e["op"] + " " + e["path"] for e in edits)
            ms = sum(_run(update_argv(binary, db_a, tree, u)) for u in ops) * 1000
            steps.append({"step": i, "edit": desc, "update_ms": round(ms, 1)})
            print(f"[{i + 1}/{len(plan)}] {desc}: {ms:.0f} ms", file=sys.stderr, flush=True)
        _run([binary, "algo", str(db_a)])
        scan(binary, tree, db_b)
        size = {"a": db_a.stat().st_size, "b": db_b.stat().st_size}
        rep = compact(compare_dbs(db_a, db_b))
    finally:
        _rm_db(db_a)
        _rm_db(db_b)
        shutil.rmtree(tree, ignore_errors=True)

    report = {"binary": binary, "binary_md5": _md5(binary), "repo": args.repo,
              "seed": args.seed, "steps": steps, "db_bytes": size,
              "equal": rep["equal"], "compare": rep}
    print(f"steps {len(steps)}, equal {rep['equal']}, differs {rep['differs']},"
          f" deps_differ {rep.get('deps_differ')}, orphan facts {rep.get('info_orphan_facts')}")
    if args.out:
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(json.dumps(report, indent=1, ensure_ascii=False) + "\n",
                                  encoding="utf-8")
    return 0 if rep["equal"] else 1


if __name__ == "__main__":
    sys.exit(main())
