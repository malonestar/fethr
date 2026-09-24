"""Persistent settings for fethr.

Settings are the single source of truth for the whole app: the engine, the tray
and the UI all read the same :class:`Settings` object, and every mutation goes
through :func:`save_settings` so a restart is a no-op.

Storage
-------
``%APPDATA%\\fethr\\settings.json`` on Windows, ``~/.config/fethr/settings.json``
elsewhere.  The file is written atomically (temp file + replace) so a crash
mid-save cannot leave a truncated JSON document behind.

Legacy import
-------------
The predecessor of this app was a single-file script with a ``config.json``
next to it.  On first run we look for that file and copy the values across
exactly once; :attr:`Settings.legacy_imported` then records the source path so
the import never repeats, even if the old file is edited afterwards.
"""

from __future__ import annotations

import json
import os
from dataclasses import MISSING, asdict, dataclass, field, fields, is_dataclass
from pathlib import Path
from typing import Any

__all__ = [
    "APP_NAME",
    "AudioSettings",
    "DictationSettings",
    "Settings",
    "UISettings",
    "find_legacy_config",
    "load_settings",
    "save_settings",
    "settings_dir",
    "settings_path",
]

APP_NAME = "fethr"

#: Keys of the old ``flow/client/config.json`` mapped onto dotted setting paths.
LEGACY_KEY_MAP: dict[str, str] = {
    "asr_url": "dictation.asr_url",
    "asr_timeout": "dictation.asr_timeout",
    "cleanup_url": "dictation.cleanup_url",
    "cleanup_model": "dictation.cleanup_model",
    "cleanup_timeout": "dictation.cleanup_timeout",
    "hotkey_raw": "dictation.hotkey_raw",
    "hotkey_clean": "dictation.hotkey_clean",
    "hotkey_repaste": "dictation.hotkey_repaste",
    "language": "dictation.language",
    "sample_rate": "dictation.sample_rate",
    "min_seconds": "dictation.min_seconds",
    "live_transcribe": "dictation.live_transcribe",
    "restore_clipboard": "dictation.restore_clipboard",
    "clipboard_restore_delay": "dictation.clipboard_restore_delay",
    "paste_after": "dictation.paste_after",
    "beeps": "dictation.beeps",
}


@dataclass
class DictationSettings:
    """Everything the dictation hot path needs.

    Defaults deliberately point at localhost: a fresh clone must not leak
    anybody's private server addresses.
    """

    asr_url: str = "http://127.0.0.1:8890"
    asr_timeout: int = 30
    language: str = "en"
    sample_rate: int = 16000
    min_seconds: float = 0.3
    #: Re-transcribe while the key is held and commit stable text as it
    #: settles (see :mod:`fethr.core.streaming`).  Off = one request per
    #: utterance, the original behaviour.
    live_transcribe: bool = True

    cleanup_enabled: bool = True
    cleanup_url: str = "http://127.0.0.1:11434"
    cleanup_model: str = "qwen3:14b"
    cleanup_timeout: int = 25

    hotkey_raw: str = "f8"
    hotkey_clean: str = "f9"
    hotkey_repaste: str = "f7"

    beeps: bool = True
    restore_clipboard: bool = True
    clipboard_restore_delay: float = 1.0
    #: Appended to every paste so the next dictation lands with a gap between
    #: them.  Typed by the user; backslash escapes (\\n \\t \\s) are honoured,
    #: see :func:`fethr.core.dictation.paste_suffix`.  Default: one space.
    paste_after: str = "\\s"


@dataclass
class AudioSettings:
    """Microphone selection and system-volume preferences."""

    #: ``None`` means "whatever Windows considers the default input device".
    input_device: str | None = None


@dataclass
class SidecarSettings:
    """Host-side preferences for the USB macro-pad (device config lives on
    the device itself, in NVS — see ``fethr.core.device``)."""

    auto_connect: bool = True
    #: Show a toast/Mono message when a transcript is pasted.
    notify_on_paste: bool = False
    #: Chain-builder canvas: where each module sits and which way it faces.
    #: A whole document rather than a field per node, because the set of nodes
    #: is whatever is plugged in. Shape and validation live in
    #: :mod:`fethr.core.layout`; ``{}`` means "never arranged".
    layout: dict = field(default_factory=dict)


@dataclass
class UISettings:
    """Window chrome and appearance."""

    theme: str = "dark"  # "dark" | "light"
    window_width: int = 960
    window_height: int = 640
    start_minimised: bool = True
    #: Float the live transcript in a small pill near the bottom of the
    #: screen while the dictation key is held (see :mod:`fethr.ui.overlay`).
    live_overlay: bool = True


@dataclass
class Settings:
    """Root settings document."""

    dictation: DictationSettings = field(default_factory=DictationSettings)
    audio: AudioSettings = field(default_factory=AudioSettings)
    sidecar: SidecarSettings = field(default_factory=SidecarSettings)
    ui: UISettings = field(default_factory=UISettings)

    #: True until the user dismisses the first-run banner.
    first_run: bool = True
    #: Absolute path of the legacy config we imported, or ``None``.
    legacy_imported: str | None = None
    #: Master switch for the USB sidecar bridge. ``False`` by default so a
    #: fresh install never scans or opens a serial port on its own; the
    #: legacy importer predates the sidecar and always leaves this ``False``.
    sidecar_enabled: bool = False

    # -- serialisation ---------------------------------------------------

    def to_dict(self) -> dict[str, Any]:
        """Return a plain JSON-serialisable dict of every setting."""
        return asdict(self)

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "Settings":
        """Build settings from ``data``, ignoring unknown keys.

        Missing keys keep their defaults, so a settings file written by an
        older version of fethr always loads.
        """
        return _build(cls, data or {})

    # -- mutation --------------------------------------------------------

    def get_path(self, path: str) -> Any:
        """Read a dotted path such as ``"dictation.asr_url"``."""
        target: Any = self
        for part in path.split("."):
            target = getattr(target, part)
        return target

    def set_path(self, path: str, value: Any) -> None:
        """Write a dotted path, coercing ``value`` to the declared type.

        Raises:
            KeyError: if the path does not name a real setting.
        """
        parts = path.split(".")
        target: Any = self
        for part in parts[:-1]:
            if not hasattr(target, part):
                raise KeyError(path)
            target = getattr(target, part)
        leaf = parts[-1]
        declared = {f.name: f.type for f in fields(target)} if is_dataclass(target) else {}
        if leaf not in declared:
            raise KeyError(path)
        setattr(target, leaf, _coerce(value, declared[leaf]))

    def update(self, patch: dict[str, Any]) -> "Settings":
        """Apply a nested or dotted ``patch`` dict in place and return self."""
        for key, value in _flatten(patch, target=self):
            try:
                self.set_path(key, value)
            except (KeyError, AttributeError):
                continue  # tolerate stale keys from an older UI build
        return self


# --------------------------------------------------------------------------
# locations
# --------------------------------------------------------------------------


def settings_dir() -> Path:
    """Return the directory holding ``settings.json`` (created on demand)."""
    override = os.environ.get("FETHR_HOME")
    if override:
        base = Path(override)
    elif os.name == "nt":
        base = Path(os.environ.get("APPDATA", Path.home() / "AppData" / "Roaming")) / APP_NAME
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / APP_NAME
    base.mkdir(parents=True, exist_ok=True)
    return base


def settings_path() -> Path:
    """Return the absolute path of the settings file."""
    return settings_dir() / "settings.json"


def find_legacy_config(extra: Path | None = None) -> Path | None:
    """Locate the predecessor's ``config.json``, or return ``None``.

    Args:
        extra: an explicit candidate checked before the built-in ones.

    Search order: ``extra``, ``$FETHR_LEGACY_CONFIG``, then ``client/config.json``
    beside the repo (the old ``flow/client`` layout) and the current directory.
    """
    candidates: list[Path] = []
    if extra:
        candidates.append(Path(extra))
    env = os.environ.get("FETHR_LEGACY_CONFIG")
    if env:
        candidates.append(Path(env))
    here = Path(__file__).resolve()
    # .../flow/fethr/fethr/core/settings.py -> parents[3] == .../flow
    for up in (2, 3):
        try:
            candidates.append(here.parents[up] / "client" / "config.json")
        except IndexError:  # pragma: no cover - only on absurd install layouts
            pass
    candidates.append(Path.cwd() / "config.json")
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    return None


# --------------------------------------------------------------------------
# load / save
# --------------------------------------------------------------------------


def load_settings(path: Path | None = None, legacy: Path | None = None) -> Settings:
    """Load settings from disk, importing the legacy config once if needed.

    Args:
        path: settings file to read; defaults to :func:`settings_path`.
        legacy: explicit legacy ``config.json`` candidate.

    Returns:
        A fully populated :class:`Settings`.  If a legacy import happened the
        result has already been written back to disk.
    """
    path = Path(path) if path else settings_path()
    if path.is_file():
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError):
            data = {}
        settings = Settings.from_dict(data)
    else:
        settings = Settings()

    if settings.legacy_imported is None:
        source = find_legacy_config(legacy)
        if source is not None and import_legacy(settings, source):
            save_settings(settings, path)
    return settings


def import_legacy(settings: Settings, source: Path) -> bool:
    """Copy values from an old ``config.json`` into ``settings``.

    Returns:
        True if the import ran (even if the file was empty), so the caller can
        persist the ``legacy_imported`` marker and never look again.
    """
    try:
        raw = json.loads(Path(source).read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return False
    if not isinstance(raw, dict):
        return False

    for old_key, dotted in LEGACY_KEY_MAP.items():
        if old_key in raw and raw[old_key] is not None:
            try:
                settings.set_path(dotted, raw[old_key])
            except (KeyError, ValueError, TypeError):
                continue
    # The old client had no on/off switch: a configured cleanup URL meant on.
    settings.dictation.cleanup_enabled = bool(raw.get("cleanup_url"))
    settings.legacy_imported = str(Path(source).resolve())
    settings.first_run = False
    return True


def save_settings(settings: Settings, path: Path | None = None) -> Path:
    """Write ``settings`` to disk atomically and return the path written."""
    path = Path(path) if path else settings_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(settings.to_dict(), indent=2), encoding="utf-8")
    os.replace(tmp, path)
    return path


# --------------------------------------------------------------------------
# internals
# --------------------------------------------------------------------------


def _build(cls: type, data: dict[str, Any]) -> Any:
    """Recursively construct dataclass ``cls`` from ``data``, skipping junk."""
    kwargs: dict[str, Any] = {}
    for f in fields(cls):
        if f.name not in data:
            continue
        value = data[f.name]
        nested = _nested_type(cls, f.name)
        if nested is not None:
            if isinstance(value, dict):
                kwargs[f.name] = _build(nested, value)
            continue
        try:
            kwargs[f.name] = _coerce(value, f.type)
        except (TypeError, ValueError):
            continue
    return cls(**kwargs)


def _nested_type(cls: type, name: str) -> type | None:
    """Return the dataclass type of field ``name``, if it is one."""
    default_factory = next(
        (f.default_factory for f in fields(cls) if f.name == name), MISSING
    )
    if default_factory is MISSING:
        return None
    try:
        candidate = default_factory()  # type: ignore[misc]
    except Exception:  # pragma: no cover - defensive
        return None
    return type(candidate) if is_dataclass(candidate) else None


def _coerce(value: Any, declared: Any) -> Any:
    """Best-effort cast of ``value`` to the type named by ``declared``.

    Field annotations arrive as strings under ``from __future__ import
    annotations``, so we match on the text rather than the type object.
    """
    text = declared if isinstance(declared, str) else getattr(declared, "__name__", "")
    optional = "None" in text
    if value is None:
        return None if optional else value
    if text.startswith("bool"):
        if isinstance(value, str):
            return value.strip().lower() in ("1", "true", "yes", "on")
        return bool(value)
    if text.startswith("int"):
        return int(value)
    if text.startswith("float"):
        return float(value)
    if text.startswith("str"):
        return str(value)
    return value


def _flatten(
    patch: dict[str, Any], prefix: str = "", target: Any = None
) -> list[tuple[str, Any]]:
    """Turn a nested dict into ``[(dotted_path, value), ...]``.

    Descends only into sections that really are nested dataclasses.  Some
    settings *are* dicts — ``sidecar.layout`` is a whole document — and
    flattening one of those would turn a single assignment into a pile of
    dotted paths that name nothing.  With no ``target`` to check against
    (a bare call), every dict is treated as a section, which is the old
    behaviour and fine for hand-written patches.
    """
    out: list[tuple[str, Any]] = []
    for key, value in patch.items():
        path = f"{prefix}{key}"
        child = getattr(target, key, None) if target is not None else None
        if isinstance(value, dict) and (target is None or is_dataclass(child)):
            out.extend(_flatten(value, prefix=f"{path}.", target=child))
        else:
            out.append((path, value))
    return out
