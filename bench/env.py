"""`.env` 로더 — 저장소 루트의 `.env`에서 환경변수를 읽는다.

`python-dotenv` 의존성을 추가하지 않는다. 필요한 건 20줄이고, 이 venv는
`--system-site-packages`로 만들어져 있어서 패키지를 하나 늘릴 때마다
"시스템 것인가 venv 것인가"가 헷갈린다.

## 규칙

- **이미 설정된 환경변수를 덮어쓰지 않는다** (`override=False`가 기본).
  쉘에서 `export`한 값이 항상 이긴다. CI나 일회성 실험에서 파일을 안 고치고
  덮어쓸 수 있어야 한다.
- 값에 따옴표가 있으면 벗긴다. `KEY="abc"`와 `KEY=abc`를 같게 취급한다.
- `#` 로 시작하는 줄과 빈 줄은 무시한다. 값 안의 `#`은 주석이 아니다 —
  API 키에 `#`이 들어갈 수 있다.

## 보안

`.env`는 `.gitignore` 대상이다. 커밋되는 것은 키가 비어 있는 `.env.example`뿐이다.
`write_env`는 파일을 0600으로 만든다.
"""

from __future__ import annotations

import os
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ENV = REPO_ROOT / ".env"


def parse_env(text: str) -> dict[str, str]:
    """`.env` 텍스트 → dict. 파일 접근 없이 테스트할 수 있게 분리했다."""
    out: dict[str, str] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, val = line.partition("=")
        key = key.strip()
        if key.startswith("export "):
            key = key[len("export "):].strip()
        if not key:
            continue
        val = val.strip()
        # 따옴표는 감쌀 때만 벗긴다. 값 중간의 따옴표는 값의 일부다.
        if len(val) >= 2 and val[0] == val[-1] and val[0] in ("'", '"'):
            val = val[1:-1]
        out[key] = val
    return out


def load_dotenv(path: Path | None = None, *, override: bool = False) -> dict[str, str]:
    """`.env`를 읽어 `os.environ`에 넣는다. 넣은 것만 돌려준다.

    파일이 없으면 조용히 `{}`. `.env`가 없는 환경(CI, 다른 사람 클론)에서
    import만으로 죽으면 안 되기 때문이다. 키가 정말 필요한 시점의 검사는
    `require()`가 한다.
    """
    p = path or DEFAULT_ENV
    if not p.exists():
        return {}
    applied = {}
    for k, v in parse_env(p.read_text(encoding="utf-8")).items():
        if override or k not in os.environ:
            os.environ[k] = v
            applied[k] = v
    return applied


def require(name: str, *, hint: str = "") -> str:
    """환경변수를 읽되, 없으면 **무엇을 해야 하는지** 알려주고 죽는다.

    `os.environ[name]`이 던지는 맨 `KeyError`로는 다음 행동을 알 수 없다.
    """
    val = os.environ.get(name)
    if val:
        return val
    raise RuntimeError(
        f"환경변수 {name} 가 비어 있다.\n"
        f"  {REPO_ROOT / '.env'} 에 `{name}=...` 를 넣거나\n"
        f"  쉘에서 `export {name}=...` 하라."
        + (f"\n  {hint}" if hint else ""))


def write_env(values: dict[str, str], path: Path | None = None) -> Path:
    """`.env`를 만들거나 갱신한다. 기존 키는 보존하고 주어진 것만 덮어쓴다.

    파일 권한을 0600으로 강제한다 — 이 파일에는 결제 수단이 연결된 키가 들어간다.
    """
    p = path or DEFAULT_ENV
    merged = parse_env(p.read_text(encoding="utf-8")) if p.exists() else {}
    merged.update(values)
    body = "".join(f"{k}={v}\n" for k, v in merged.items())
    p.write_text(body, encoding="utf-8")
    p.chmod(0o600)
    return p
