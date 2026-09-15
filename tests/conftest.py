"""Make the repository importable without installing it.

``pytest`` puts ``tests/`` on ``sys.path`` but not the project root, and fethr
is deliberately runnable straight from a clone (``python -m fethr``), so the
tests import it the same way a user would.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


@pytest.fixture(autouse=True)
def isolate_legacy_lookup(monkeypatch):
    """Stop :func:`load_settings` discovering a real ``config.json``.

    The built-in search deliberately looks beside the repository, which on a
    developer's machine is a live file. Tests that exercise the import pass an
    explicit candidate; every other test must see "no legacy config".
    """
    from fethr.core import settings as settings_module

    def only_explicit(extra: Path | None = None) -> Path | None:
        return Path(extra).resolve() if extra and Path(extra).is_file() else None

    monkeypatch.setattr(settings_module, "find_legacy_config", only_explicit)
