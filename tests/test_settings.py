"""Settings: defaults, round-tripping, coercion and the one-time legacy import."""

from __future__ import annotations

import json

import pytest

from fethr.core.settings import (
    Settings,
    find_legacy_config,
    import_legacy,
    load_settings,
    save_settings,
    settings_dir,
)

# The exact config.json shipped with the predecessor client.
LEGACY_CONFIG = {
    "asr_url": "http://192.168.1.50:8890",
    "cleanup_url": "http://192.168.1.60:11434",
    "cleanup_model": "hf.co/Qwen/Qwen3-14B-GGUF:Q5_K_M",
    "hotkey_raw": "f8",
    "hotkey_clean": "f9",
    "hotkey_repaste": "f7",
    "language": "en",
    "sample_rate": 16000,
    "min_seconds": 0.3,
    "asr_timeout": 30,
    "cleanup_timeout": 25,
    "restore_clipboard": True,
    "clipboard_restore_delay": 1.0,
    "beeps": True,
}


# ---------------------------------------------------------------- defaults --


def test_defaults_are_local_only():
    """A fresh install must not point at anybody's private servers."""
    settings = Settings()
    assert settings.dictation.asr_url == "http://127.0.0.1:8890"
    assert settings.dictation.cleanup_url == "http://127.0.0.1:11434"
    assert settings.dictation.cleanup_model == "qwen3:14b"
    assert settings.first_run is True
    assert settings.legacy_imported is None


def test_sidecar_disabled_by_default():
    """A fresh install must never scan for the USB sidecar unasked."""
    settings = Settings()
    assert settings.sidecar_enabled is False


def test_legacy_import_leaves_the_sidecar_disabled(tmp_path):
    """The predecessor client predates the sidecar; import must not turn it on."""
    legacy = tmp_path / "config.json"
    legacy.write_text(json.dumps(LEGACY_CONFIG), encoding="utf-8")
    settings = Settings()
    assert import_legacy(settings, legacy) is True
    assert settings.sidecar_enabled is False


def test_default_hotkeys_match_the_original_client():
    settings = Settings()
    assert (settings.dictation.hotkey_raw,
            settings.dictation.hotkey_clean,
            settings.dictation.hotkey_repaste) == ("f8", "f9", "f7")


# ------------------------------------------------------------ persistence --


def test_save_and_load_round_trip(tmp_path):
    path = tmp_path / "settings.json"
    settings = Settings()
    settings.dictation.asr_url = "http://192.168.0.50:8890"
    settings.ui.theme = "light"
    settings.legacy_imported = "already-done"  # stop the loader hunting for one
    save_settings(settings, path)

    reloaded = load_settings(path)
    assert reloaded.dictation.asr_url == "http://192.168.0.50:8890"
    assert reloaded.ui.theme == "light"


def test_load_tolerates_corrupt_json(tmp_path):
    path = tmp_path / "settings.json"
    path.write_text("{not json at all", encoding="utf-8")
    settings = load_settings(path, legacy=tmp_path / "missing.json")
    assert settings.dictation.asr_url == "http://127.0.0.1:8890"


def test_unknown_and_missing_keys_are_tolerated():
    settings = Settings.from_dict(
        {
            "dictation": {"asr_url": "http://x:1", "who_knows": 12},
            "obsolete_section": {"a": 1},
        }
    )
    assert settings.dictation.asr_url == "http://x:1"
    assert settings.dictation.language == "en"  # default survived
    assert not hasattr(settings, "obsolete_section")


def test_settings_dir_honours_fethr_home(tmp_path, monkeypatch):
    monkeypatch.setenv("FETHR_HOME", str(tmp_path / "portable"))
    assert settings_dir() == tmp_path / "portable"
    assert (tmp_path / "portable").is_dir()


# ------------------------------------------------------- paths & coercion --


def test_set_path_coerces_to_the_declared_type():
    settings = Settings()
    settings.set_path("dictation.sample_rate", "48000")
    settings.set_path("dictation.beeps", "false")
    settings.set_path("dictation.min_seconds", "0.75")
    assert settings.dictation.sample_rate == 48000
    assert settings.dictation.beeps is False
    assert settings.dictation.min_seconds == pytest.approx(0.75)


def test_set_path_rejects_unknown_paths():
    with pytest.raises(KeyError):
        Settings().set_path("dictation.nope", 1)


def test_update_applies_a_nested_patch_and_skips_junk():
    settings = Settings()
    settings.update(
        {
            "dictation": {"cleanup_enabled": False, "bogus": 1},
            "ui": {"theme": "light"},
            "nonsense": {"deep": {"deeper": 2}},
        }
    )
    assert settings.dictation.cleanup_enabled is False
    assert settings.ui.theme == "light"


def test_optional_field_accepts_none():
    settings = Settings()
    settings.set_path("audio.input_device", "Yeti")
    assert settings.audio.input_device == "Yeti"
    settings.set_path("audio.input_device", None)
    assert settings.audio.input_device is None


# ------------------------------------------------------------ legacy import --


def test_legacy_config_is_imported_once(tmp_path):
    legacy = tmp_path / "config.json"
    legacy.write_text(json.dumps(LEGACY_CONFIG), encoding="utf-8")
    path = tmp_path / "settings.json"

    settings = load_settings(path, legacy=legacy)
    assert settings.dictation.asr_url == LEGACY_CONFIG["asr_url"]
    assert settings.dictation.cleanup_url == LEGACY_CONFIG["cleanup_url"]
    assert settings.dictation.cleanup_model == LEGACY_CONFIG["cleanup_model"]
    assert settings.dictation.cleanup_enabled is True
    assert settings.first_run is False
    assert settings.legacy_imported == str(legacy.resolve())
    assert path.is_file(), "the import must be persisted immediately"

    # Editing the old file afterwards must not leak back in.
    legacy.write_text(json.dumps({**LEGACY_CONFIG, "asr_url": "http://changed"}),
                      encoding="utf-8")
    again = load_settings(path, legacy=legacy)
    assert again.dictation.asr_url == LEGACY_CONFIG["asr_url"]


def test_legacy_import_without_cleanup_url_disables_cleanup(tmp_path):
    legacy = tmp_path / "config.json"
    legacy.write_text(json.dumps({"asr_url": "http://a:8890"}), encoding="utf-8")

    settings = Settings()
    assert import_legacy(settings, legacy) is True
    assert settings.dictation.cleanup_enabled is False
    assert settings.dictation.asr_url == "http://a:8890"


def test_legacy_import_ignores_a_corrupt_file(tmp_path):
    legacy = tmp_path / "config.json"
    legacy.write_text("[]", encoding="utf-8")
    settings = Settings()
    assert import_legacy(settings, legacy) is False
    assert settings.legacy_imported is None


def test_find_legacy_config_prefers_the_explicit_candidate(tmp_path, monkeypatch):
    monkeypatch.delenv("FETHR_LEGACY_CONFIG", raising=False)
    explicit = tmp_path / "config.json"
    explicit.write_text("{}", encoding="utf-8")
    assert find_legacy_config(explicit) == explicit.resolve()


def test_find_legacy_config_uses_the_environment_override(tmp_path, monkeypatch):
    target = tmp_path / "elsewhere.json"
    target.write_text("{}", encoding="utf-8")
    monkeypatch.setenv("FETHR_LEGACY_CONFIG", str(target))
    assert find_legacy_config() == target.resolve()
