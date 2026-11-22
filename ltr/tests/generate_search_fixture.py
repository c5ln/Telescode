"""Generate the Python reference fixture for TypeScript tokenizer/BM25 parity.

Run from the repository root:
    bench/.venv/bin/python -m ltr.tests.generate_search_fixture

Imports only `ltr.retrieval` (no pandas/numpy).
"""

from __future__ import annotations

import json
from pathlib import Path

from ltr.retrieval.bm25 import B_DEFAULT, BM25, K1_DEFAULT
from ltr.retrieval.tokenizer import tokenize, tokenize_path

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "frontend/src/search/fixtures/bm25.fixture.json"

LONG_BLOCK = (
    "def compute_metric_score(alpha_weight, beta_weight, sample_count):\n"
    "    total_value = alpha_weight * sample_count + beta_weight\n"
    "    return total_value / max(sample_count, 1)\n\n"
)

FILES: list[dict[str, str]] = [
    {
        "fileId": "src/core/query_notify.py",
        "source": (
            "class QueryNotify:\n"
            "    def __init__(self, result_store):\n"
            "        self.result_store = result_store\n"
            "    def notifyUsers(self, status_code):\n"
            "        return self.get_status_code(status_code)\n"
            "    def get_status_code(self, code):\n"
            "        return code\n"
        ),
    },
    {
        "fileId": "src/http/HTMLParser.ts",
        "source": (
            "export class HTMLParser {\n"
            "  getHTTPResponse(url: string) {\n"
            "    const server = new HTTP2Server()\n"
            "    const req = new XMLHttpRequest()\n"
            "    return server.dispatch(req, url)\n"
            "  }\n"
            "}\n"
        ),
    },
    {
        "fileId": "src/internal/_private_cache.py",
        "source": (
            "class _PrivateCache:\n"
            "    def __init__(self):\n"
            "        self._raw_items = {}\n"
            "        self.__hidden_key__ = '___'\n"
            "        x = 1\n"
            "        a_b = x + 2\n"
            "        self._evict_stale_entries(a_b)\n"
        ),
    },
    {
        "fileId": "src/errors/error_type.py",
        "source": (
            "class FileError(Exception):\n"
            "    def __init__(self, error_type, file_path):\n"
            "        self.type = error_type\n"
            "        self.file = file_path\n"
            "    def describe_issue(self):\n"
            "        return f'python error in file: {self.type}'\n"
        ),
    },
    {
        "fileId": "src/math/numbers_v2.py",
        "source": (
            "HTTP_NOT_FOUND = 404\n"
            "MAX_BUFFER = 32768\n"
            "def base64_encode_v2(payload, sha256_digest):\n"
            "    version = 2\n"
            "    return f'{version}:{sha256_digest}:{HTTP_NOT_FOUND}:{MAX_BUFFER}'\n"
        ),
    },
    {
        "fileId": "src/docs/long_document.py",
        "source": (
            "# Long document to exercise BM25 document-length normalization.\n"
            + LONG_BLOCK * 12
            + "def notify_metric_summary(status_code):\n"
            "    return compute_metric_score(1, 2, status_code)\n"
        ),
    },
    {
        "fileId": "src/empty_only_path-test.py",
        "source": "",
    },
]

QUERIES: list[str] = [
    "QueryNotify get_status",
    "HTMLParser HTTP2 XMLHttpRequest",
    "error type class file",
    "_private __init__ ___ a_b",
    "notify notify status",
    "status notify notify",
    "404 sha256 base64 v2 x",
    "empty path test",
    "compute_metric_score alpha_weight",
    "nonexistent_token_xyz",
    "",
]

IDF_PROBE_TERMS: list[str] = [
    "src",
    "notify",
    "error",
    "html",
    "404",
    "nonexistent_token_xyz",
]


def main() -> None:
    file_ids = [f["fileId"] for f in FILES]
    docs = [
        tokenize_path(f["fileId"]) + tokenize(f["source"], keep_stopwords=True)
        for f in FILES
    ]
    index = BM25.build(file_ids, docs, k1=K1_DEFAULT, b=B_DEFAULT, k3=None)

    fixture = {
        "generator": "bench/.venv/bin/python -m ltr.tests.generate_search_fixture",
        "params": {"k1": K1_DEFAULT, "b": B_DEFAULT, "k3": None},
        "files": [
            {
                "fileId": f["fileId"],
                "source": f["source"],
                "pathTokens": tokenize_path(f["fileId"]),
                "sourceTokensKeepStopwords": tokenize(f["source"], keep_stopwords=True),
                "sourceTokensDefault": tokenize(f["source"]),
            }
            for f in FILES
        ],
        "idfs": {term: index.idf(term) for term in IDF_PROBE_TERMS},
        "queries": [
            {
                "query": q,
                "tokensKeepStopwords": tokenize(q, keep_stopwords=True),
                "tokensDefault": tokenize(q),
                "scores": index.score_query(tokenize(q, keep_stopwords=True)),
            }
            for q in QUERIES
        ],
    }

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(json.dumps(fixture, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
