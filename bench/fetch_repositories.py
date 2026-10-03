"""Prepare the external Git repositories used to build LTR features.

Repository contents are intentionally not committed to Telescode.  Their
remote URLs live in ``repositories.json`` and the exact commits required by
the training set live in ``data/instances_all.csv`` as ``base_commit``.

Usage:
    python bench/fetch_repositories.py
    python bench/fetch_repositories.py --verify-only
"""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
from pathlib import Path


BENCH_DIR = Path(__file__).resolve().parent
DEFAULT_MANIFEST = BENCH_DIR / "repositories.json"
DEFAULT_INSTANCES = BENCH_DIR / "data" / "instances_all.csv"


def run_git(repo: Path | None, *args: str, capture: bool = False) -> str:
    command = ["git"]
    if repo is not None:
        command += ["-C", str(repo)]
    command += list(args)
    result = subprocess.run(
        command,
        check=True,
        text=True,
        stdout=subprocess.PIPE if capture else None,
    )
    return result.stdout.strip() if capture else ""


def required_commits(instances: Path, prefix: str) -> set[str]:
    with instances.open(newline="", encoding="utf-8") as stream:
        rows = csv.DictReader(stream)
        required = {"instance_id", "base_commit"}
        missing = required - set(rows.fieldnames or ())
        if missing:
            raise ValueError(f"{instances} is missing columns: {sorted(missing)}")
        return {
            row["base_commit"]
            for row in rows
            if row["instance_id"].startswith(prefix) and row["base_commit"]
        }


def has_commit(repo: Path, commit: str) -> bool:
    result = subprocess.run(
        ["git", "-C", str(repo), "cat-file", "-e", f"{commit}^{{commit}}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return result.returncode == 0


def prepare_repository(name: str, spec: dict[str, str], instances: Path,
                       verify_only: bool) -> None:
    url = spec["url"]
    repo = (BENCH_DIR / spec["path"]).resolve()
    commits = required_commits(instances, spec["instance_prefix"])
    if not commits:
        raise RuntimeError(f"{name}: no required commits found in {instances}")

    if not repo.exists():
        if verify_only:
            raise RuntimeError(f"{name}: repository is missing: {repo}")
        repo.parent.mkdir(parents=True, exist_ok=True)
        run_git(None, "clone", "--no-checkout", url, str(repo))

    if run_git(repo, "rev-parse", "--is-inside-work-tree", capture=True) != "true":
        raise RuntimeError(f"{name}: not a Git worktree: {repo}")

    actual_url = run_git(repo, "remote", "get-url", "origin", capture=True)
    if actual_url.rstrip("/").removesuffix(".git") != url.rstrip("/").removesuffix(".git"):
        raise RuntimeError(
            f"{name}: origin URL mismatch: expected {url!r}, got {actual_url!r}")

    if not verify_only:
        run_git(repo, "fetch", "--tags", "--prune", "origin")

    missing = sorted(commit for commit in commits if not has_commit(repo, commit))
    if missing and not verify_only:
        for commit in missing:
            run_git(repo, "fetch", "origin", commit)
        missing = sorted(commit for commit in commits if not has_commit(repo, commit))
    if missing:
        raise RuntimeError(
            f"{name}: {len(missing)} required commits are unavailable; first={missing[0]}")

    print(f"{name}: {len(commits)} required base commits available in {repo}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--instances", type=Path, default=DEFAULT_INSTANCES)
    parser.add_argument(
        "--verify-only", action="store_true",
        help="perform no clone/fetch; only validate URLs and required commits",
    )
    args = parser.parse_args(argv)

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if manifest.get("schema_version") != 1:
        raise ValueError(f"unsupported repository manifest: {args.manifest}")
    for name, spec in manifest["repositories"].items():
        prepare_repository(name, spec, args.instances, args.verify_only)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
