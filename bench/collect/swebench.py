"""SWE-bench 인스턴스 로딩 + gold 파일 추출.

`datasets` 패키지 없이 `huggingface_hub`로 parquet만 내려받아 pyarrow로 읽는다.
의존성을 하나 줄이고, 캐시가 HF 캐시에 그대로 남아 재실행이 빠르다.
"""

import re
from dataclasses import dataclass, field

import pandas as pd
import pyarrow.parquet as pq
from huggingface_hub import hf_hub_download

from bench.schema import normalize_file_id

SWEBENCH_REPO = "princeton-nlp/SWE-bench"

_DIFF_GIT = re.compile(r"^diff --git a/(.+?) b/(.+)$")


def load_instances(repo: str | None = None, split: str = "test") -> pd.DataFrame:
    """SWE-bench 한 split을 DataFrame으로. `repo`를 주면 그 repo만 남긴다."""
    path = hf_hub_download(
        SWEBENCH_REPO, f"data/{split}-00000-of-00001.parquet", repo_type="dataset")
    df = pq.read_table(path).to_pandas()
    if repo is not None:
        df = df[df["repo"] == repo].reset_index(drop=True)
    return df


@dataclass
class GoldFiles:
    """gold patch가 건드린 파일들을 base_commit 시점 존재 여부로 나눈 것.

    `added` 파일은 base_commit에 **존재하지 않으므로** 어떤 검색기도 반환할 수
    없다. gold에는 계약대로 포함시키되(`all`), `reachable_recall_ceiling`이
    그만큼 1 미만으로 떨어진다. 그 손실이 파서 실패 때문인지 신생 파일 때문인지
    구분하려고 여기서 미리 쪼개 둔다.
    """

    modified: set[str] = field(default_factory=set)
    added: set[str] = field(default_factory=set)
    deleted: set[str] = field(default_factory=set)
    renamed_from: set[str] = field(default_factory=set)

    @property
    def all(self) -> set[str]:
        return self.modified | self.added | self.deleted | self.renamed_from

    @property
    def existing_at_base(self) -> set[str]:
        """base_commit에 실제로 있던 경로만."""
        return self.modified | self.deleted | self.renamed_from


def gold_files_from_patch(patch: str) -> GoldFiles:
    """unified diff에서 gold 파일 경로를 뽑는다.

    `--- a/… / +++ b/…` 쌍을 본다. `diff --git` 헤더만 보면 rename이나
    삭제를 구분할 수 없다.

      - `--- /dev/null`  → 신규 파일 (base_commit에 없음)
      - `+++ /dev/null`  → 삭제 파일 (base_commit에 있음, a 경로 사용)
      - a ≠ b            → rename. base_commit에 있는 건 **a 경로**다
      - 그 외            → 수정
    """
    out = GoldFiles()
    lines = patch.splitlines() if patch else []
    i = 0
    while i < len(lines):
        if not lines[i].startswith("--- "):
            i += 1
            continue
        if i + 1 >= len(lines) or not lines[i + 1].startswith("+++ "):
            i += 1
            continue

        a_raw = lines[i][4:].split("\t")[0].strip()
        b_raw = lines[i + 1][4:].split("\t")[0].strip()
        i += 2

        a_null = a_raw == "/dev/null"
        b_null = b_raw == "/dev/null"
        a = normalize_file_id(a_raw[2:]) if a_raw.startswith("a/") else normalize_file_id(a_raw)
        b = normalize_file_id(b_raw[2:]) if b_raw.startswith("b/") else normalize_file_id(b_raw)

        if a_null and b_null:
            continue
        if a_null:
            out.added.add(b)
        elif b_null:
            out.deleted.add(a)
        elif a != b:
            out.renamed_from.add(a)
        else:
            out.modified.add(b)
    return out
