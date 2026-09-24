"""The text path: transcript scrubbing, think-tag stripping and the event bus.

None of these tests touch a microphone, a GPU or the network — the engine's
HTTP session is swapped for a stub, which is enough to exercise every line of
text handling between whisper.cpp and the clipboard.
"""

from __future__ import annotations

import io
import wave

import pytest

from fethr.core.dictation import (
    DictationEngine,
    EngineState,
    EventBus,
    scrub_transcript,
    silent_wav,
    strip_think,
)
from fethr.core.settings import Settings


# ------------------------------------------------------- scrub_transcript --


@pytest.mark.parametrize(
    "raw, expected",
    [
        ("", ""),
        ("hello world", "hello world"),
        # whisper.cpp puts each segment on its own line
        ("first segment\nsecond segment\n", "first segment second segment"),
        # short clips sometimes come back formatted as dialogue
        ("- Hello there.\n- General Kenobi.", "Hello there. General Kenobi."),
        ("   leading and trailing   ", "leading and trailing"),
        ("tabs\tand spaces   collapse", "tabs and spaces collapse"),
        ("-  spaced marker", "spaced marker"),
        # a hyphen that is NOT a dialogue marker must survive
        ("well-known set-up", "well-known set-up"),
        ("a - b", "a - b"),
    ],
)
def test_scrub_transcript(raw, expected):
    assert scrub_transcript(raw) == expected


def test_scrub_transcript_handles_none_like_input():
    assert scrub_transcript(None) == ""


# ------------------------------------------------------------ strip_think --


@pytest.mark.parametrize(
    "raw, expected",
    [
        ("clean output", "clean output"),
        ("<think>weighing it up</think>the answer", "the answer"),
        ("<think>\nmulti\nline\n</think>\n\nfinal text", "final text"),
        ("stray </think> tag", "stray  tag"),
        ("<think>only reasoning</think>", ""),
        ("", ""),
    ],
)
def test_strip_think(raw, expected):
    assert strip_think(raw) == expected


def test_strip_think_removes_several_blocks():
    assert strip_think("<think>a</think>one<think>b</think>two") == "onetwo"


# -------------------------------------------------------------- silent_wav --


def test_silent_wav_is_a_valid_mono_16k_clip():
    data = silent_wav(0.5, 16000)
    with wave.open(io.BytesIO(data), "rb") as handle:
        assert handle.getnchannels() == 1
        assert handle.getsampwidth() == 2
        assert handle.getframerate() == 16000
        assert handle.getnframes() == 8000


# --------------------------------------------------------------- EventBus --


def test_event_bus_delivers_to_subscribers():
    bus = EventBus()
    seen: list[dict] = []
    bus.subscribe(seen.append)
    bus.emit({"type": "state", "state": "recording"})
    _wait(lambda: len(seen) == 1)
    assert seen[0]["state"] == "recording"
    bus.close()


def test_event_bus_survives_a_broken_subscriber():
    bus = EventBus()
    good: list[dict] = []

    def explode(_event: dict) -> None:
        raise RuntimeError("subscriber is broken")

    bus.subscribe(explode)
    bus.subscribe(good.append)
    bus.emit({"type": "state", "state": "idle"})
    _wait(lambda: len(good) == 1)
    assert good[0]["state"] == "idle"
    bus.close()


def test_unsubscribe_stops_delivery():
    bus = EventBus()
    seen: list[dict] = []
    cancel = bus.subscribe(seen.append)
    cancel()
    bus.emit({"type": "state", "state": "idle"})
    _wait(lambda: False, timeout=0.2)
    assert seen == []
    bus.close()


# ------------------------------------------------- engine text round trip --


class StubResponse:
    """Minimal stand-in for a ``requests`` response."""

    def __init__(self, payload, status=200):
        self._payload = payload
        self.status_code = status

    def raise_for_status(self) -> None:
        if self.status_code >= 400:
            raise RuntimeError(f"HTTP {self.status_code}")

    def json(self):
        return self._payload


class StubSession:
    """Records posts and replays queued responses."""

    def __init__(self, responses):
        self._responses = list(responses)
        self.posts: list[tuple[str, dict]] = []

    def post(self, url, **kwargs):
        self.posts.append((url, kwargs))
        response = self._responses.pop(0)
        if isinstance(response, Exception):
            raise response
        return response


@pytest.fixture
def engine():
    """An engine with no hotkeys bound and a stubbed HTTP session."""
    settings = Settings()
    settings.dictation.beeps = False
    return DictationEngine(settings, EventBus())


def test_transcribe_scrubs_the_server_response(engine):
    engine._session = StubSession([StubResponse({"text": "- one\n- two\n"})])
    assert engine.transcribe(b"wav") == "one two"
    url, kwargs = engine._session.posts[0]
    assert url == "http://127.0.0.1:8890/inference"
    assert kwargs["data"]["language"] == "en"
    assert kwargs["data"]["response_format"] == "json"


def test_transcribe_uses_the_configured_url_without_a_double_slash(engine):
    engine.settings.dictation.asr_url = "http://box:8890/"
    engine._session = StubSession([StubResponse({"text": "hi"})])
    engine.transcribe(b"wav")
    assert engine._session.posts[0][0] == "http://box:8890/inference"


def test_cleanup_strips_reasoning(engine):
    engine._session = StubSession(
        [StubResponse({"message": {"content": "<think>hmm</think>Polished text."}})]
    )
    assert engine.cleanup("polished text") == "Polished text."


def test_cleanup_falls_back_to_the_raw_text_on_failure(engine):
    engine._session = StubSession([RuntimeError("ollama is down")])
    assert engine.cleanup("raw words") == "raw words"


def test_cleanup_falls_back_when_the_model_returns_nothing(engine):
    engine._session = StubSession([StubResponse({"message": {"content": "<think>x</think>"}})])
    assert engine.cleanup("raw words") == "raw words"


def test_test_transcribe_reports_failures_instead_of_raising(engine):
    engine._session = StubSession([RuntimeError("connection refused")])
    result = engine.test_transcribe(b"wav")
    assert result["ok"] is False
    assert "connection refused" in result["error"]


def test_snapshot_reflects_configured_hotkeys(engine):
    engine.settings.dictation.hotkey_raw = "f4"
    snapshot = engine.snapshot()
    assert snapshot["state"] == EngineState.IDLE.value
    assert snapshot["hotkeys"]["raw"] == "f4"
    assert snapshot["bound"] is False


def _wait(predicate, timeout: float = 2.0) -> None:
    """Spin until ``predicate`` is true or the timeout expires."""
    import time

    deadline = time.time() + timeout
    while time.time() < deadline and not predicate():
        time.sleep(0.005)


# ------------------------------------------------------ cleanup guard --

from fethr.core.dictation import CLEANUP_EXAMPLES, cleanup_looks_sane  # noqa: E402


@pytest.mark.parametrize(
    "raw, cleaned, ok",
    [
        ("explain to me what multiplexing is", "Explain to me what multiplexing is.", True),
        ("um so like the build is green", "So the build is green.", True),
        ("hi", "Hi.", True),
        ("hi", "Hi there, how can I help you today?", False),
        ("explain to me what multiplexing is",
         "Multiplexing is a technique that combines several signals into one "
         "shared medium so that a single channel can carry many streams at once.", False),
        ("explain to me what multiplexing is", "", False),
        ("explain to me what multiplexing is", "   ", False),
        ("one two three four five six seven eight nine ten", "One.", False),
        ("", "Anything.", False),
    ],
)
def test_cleanup_looks_sane(raw, cleaned, ok):
    assert cleanup_looks_sane(raw, cleaned) is ok


def test_cleanup_examples_pass_their_own_guard():
    for spoken, tidied in CLEANUP_EXAMPLES:
        assert cleanup_looks_sane(spoken, tidied), spoken


def test_cleanup_rejects_an_answer(monkeypatch):
    """The engine must paste the raw words when the model answers instead of edits."""
    from fethr.core.dictation import DictationEngine
    from fethr.core.settings import Settings

    engine = DictationEngine(Settings())
    sent = {}

    class _Resp:
        def raise_for_status(self):
            pass

        def json(self):
            return {"message": {"content": "Multiplexing is a way to " * 8}}

    def fake_post(url, json=None, timeout=None, **kw):
        sent["json"] = json
        return _Resp()

    monkeypatch.setattr(engine._session, "post", fake_post)
    raw = "explain to me what multiplexing is"
    assert engine.cleanup(raw) == raw
    roles = [m["role"] for m in sent["json"]["messages"]]
    assert roles[0] == "system" and roles[-1] == "user"
    assert roles.count("assistant") == len(CLEANUP_EXAMPLES) + 1  # + the context example
    assert sent["json"]["messages"][-1]["content"] == raw  # no context -> bare transcript


def test_inject_appends_paste_suffix(monkeypatch):
    import fethr.core.dictation as d
    from fethr.core.settings import Settings

    copied = []
    monkeypatch.setattr(d, "pyperclip", type("P", (), {
        "copy": staticmethod(lambda t: copied.append(t)),
        "paste": staticmethod(lambda: "old"),
    }))
    monkeypatch.setattr(d, "keyboard", type("K", (), {"send": staticmethod(lambda k: None)}))
    s = Settings()
    s.dictation.restore_clipboard = False
    engine = d.DictationEngine(s)
    engine.inject("hello")
    assert copied[-1] == "hello "
    s.dictation.paste_after = "\\n"
    engine.inject("hello")
    assert copied[-1] == "hello\n"
    s.dictation.paste_after = ""
    engine.inject("hello")
    assert copied[-1] == "hello"


@pytest.mark.parametrize("value, expected", [
    ("space", " "), ("newline", "\n"), ("none", ""), (None, ""),
    ("\\s", " "), ("\\n", "\n"), ("\\n\\n", "\n\n"), ("\\t", "\t"),
    (" ", " "), ("", ""), (" -- ", " -- "), ("\\\\", "\\"), ("\\x", "\\x"),
])
def test_paste_suffix(value, expected):
    from fethr.core.dictation import paste_suffix
    assert paste_suffix(value) == expected


# ------------------------------------------- cleanup request shape (live) --


class _RecordingSession:
    """Records posted JSON bodies; replies with a fixed cleaned text."""

    def __init__(self, reply: str = "Fine."):
        self.bodies: list[dict] = []
        self.reply = reply

    def post(self, url, json=None, timeout=None, **kw):
        self.bodies.append(json)
        reply = self.reply

        class _R:
            def raise_for_status(self):
                pass

            def json(self):
                return {"message": {"content": reply}}

        return _R()


def test_cleanup_sends_context_marked_do_not_repeat_and_guards_transcript_only():
    from fethr.core.dictation import DictationEngine
    from fethr.core.settings import Settings

    engine = DictationEngine(Settings())
    engine._session = _RecordingSession("And it built green.")
    context = "I pushed the fix to main. " * 6  # far longer than the transcript
    assert engine.cleanup("and uh it built green", context=context) == "And it built green."
    body = engine._session.bodies[0]
    last = body["messages"][-1]["content"]
    assert last.startswith("Context (do not repeat): I pushed the fix")
    assert last.endswith("\nTranscript: and uh it built green")
    assert "\n" not in last.split("\nTranscript:")[0]  # context collapsed to one line


def test_cleanup_request_options_are_shared_box_safe():
    from fethr.core.dictation import CLEANUP_KEEP_ALIVE, DictationEngine, cleanup_num_predict
    from fethr.core.settings import Settings

    engine = DictationEngine(Settings())
    engine._session = _RecordingSession("Hello there.")
    engine.cleanup("hello there")
    body = engine._session.bodies[0]
    assert body["think"] is False
    assert body["keep_alive"] == CLEANUP_KEEP_ALIVE == "10m"
    assert body["keep_alive"] != -1
    assert body["options"]["num_predict"] == cleanup_num_predict("hello there")


def test_cleanup_num_predict_scales_with_input():
    from fethr.core.dictation import cleanup_num_predict

    assert cleanup_num_predict("") == 16
    short, long = cleanup_num_predict("a b c"), cleanup_num_predict("word " * 100)
    assert short < long
    assert long >= 2 * (500 // 4)  # never below ~2x a generous token estimate


def test_cleanup_prompt_prefix_is_identical_across_requests():
    """The shared prefix is what Ollama caches and the warm-up primes."""
    from fethr.core.dictation import cleanup_messages

    a = cleanup_messages("one thing")
    b = cleanup_messages("another thing", context="Earlier text.")
    assert a[:-1] == b[:-1]


def test_warm_cleanup_is_tiny_and_throttled(monkeypatch):
    import time as _time

    from fethr.core import dictation as d
    from fethr.core.settings import Settings

    engine = d.DictationEngine(Settings())
    engine._session = _RecordingSession("ok")
    assert engine.warm_cleanup() is True
    deadline = _time.time() + 2
    while not engine._session.bodies and _time.time() < deadline:
        _time.sleep(0.005)
    body = engine._session.bodies[0]
    assert body["options"]["num_predict"] == 1
    assert body["keep_alive"] == "10m"
    assert body["messages"][0]["content"] == d.CLEANUP_SYSTEM
    assert engine.warm_cleanup() is False  # within 5 minutes: skipped
    engine._last_warmup -= d.WARMUP_INTERVAL_S + 1
    assert engine.warm_cleanup() is True


def test_press_in_clean_mode_warms_up_but_raw_mode_does_not():
    from fethr.core.dictation import DictationEngine, EventBus
    from fethr.core.settings import Settings

    settings = Settings()
    settings.dictation.beeps = False
    settings.dictation.live_transcribe = False
    engine = DictationEngine(settings, EventBus())
    warmed: list[bool] = []
    engine.warm_cleanup = lambda: warmed.append(True) or True  # type: ignore[method-assign]

    class _Mic:
        on_frames = None

        def start(self):
            pass

        def stop(self):
            return None

    engine.recorder = _Mic()  # type: ignore[assignment]
    engine.on_press("raw")
    engine.on_release("raw")
    assert warmed == []
    engine.on_press("clean")
    engine.on_release("clean")
    assert warmed == [True]
    settings.dictation.cleanup_enabled = False
    engine.on_press("clean")
    engine.on_release("clean")
    assert warmed == [True]
