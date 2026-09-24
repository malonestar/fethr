"""Live transcription: LocalAgreement, sentence cutting and the threaded driver.

Nothing here needs a microphone or a server.  The end-to-end tests drive
:class:`LiveTranscriber` with fake ``transcribe_fn`` implementations:

* a *scripted* fake that returns progressively longer hypotheses regardless of
  the audio (checks commit order, cleanup and ``finish``), and
* an *audio-aware* fake where every "word" is a block of samples whose value is
  the word's index, so the transcript of a window is exactly the words whose
  audio it contains (checks that moving the window forward neither drops nor
  duplicates words).
"""

from __future__ import annotations

import threading
import time

import numpy as np
import pytest

from fethr.core.streaming import (
    LiveTranscriber,
    LiveTranscript,
    local_agreement,
    sentence_boundaries,
    split_cleaned,
    strip_overlap,
    tokenize,
)


# ------------------------------------------------------------- tokenize --


def test_tokenize_keeps_punctuation_attached():
    assert tokenize("Hello, there.  How are you?") == ["Hello,", "there.", "How", "are", "you?"]


def test_tokenize_drops_whisper_annotations():
    assert tokenize("[BLANK_AUDIO]") == []
    assert tokenize("hi [Music] there") == ["hi", "there"]
    assert tokenize("") == []


# ------------------------------------------------------ local_agreement --


def test_agreement_with_no_previous_hypothesis_commits_nothing():
    assert local_agreement([], ["hello", "there"], []) == ([], ["hello", "there"])


def test_agreement_empty_new_hypothesis():
    assert local_agreement(["hello"], [], []) == ([], [])


def test_agreement_growing_hypothesis_commits_the_shared_prefix():
    newly, tentative = local_agreement(["hello", "there"], ["hello", "there", "how", "are"], [])
    assert newly == ["hello", "there"]
    assert tentative == ["how", "are"]


def test_agreement_skips_words_already_committed():
    prev = ["hello", "there", "how", "are"]
    new = ["hello", "there", "how", "are", "you"]
    newly, tentative = local_agreement(prev, new, ["hello", "there"])
    assert newly == ["how", "are"]
    assert tentative == ["you"]


def test_agreement_tail_revision_stays_tentative():
    prev = ["I", "want", "to", "wreck", "a", "nice"]
    new = ["I", "want", "to", "recognise", "speech"]
    newly, tentative = local_agreement(prev, new, [])
    assert newly == ["I", "want", "to"]
    assert tentative == ["recognise", "speech"]


def test_agreement_ignores_case_and_punctuation_but_emits_newest_spelling():
    prev = ["hello", "there", "how"]
    new = ["Hello,", "there.", "How", "are"]
    newly, tentative = local_agreement(prev, new, [])
    assert newly == ["Hello,", "there.", "How"]
    assert tentative == ["are"]


def test_agreement_punctuation_only_words_must_match_exactly():
    newly, _ = local_agreement(["-", "a"], ["--", "a"], [])
    assert newly == []


# -------------------------------------------------- sentence_boundaries --


@pytest.mark.parametrize(
    "words, cuts",
    [
        ([], []),
        (["no", "end"], []),
        (["Hi."], [1]),
        (["Hi.", "How", "are", "you?", "Fine"], [1, 4]),
        (["Wow!", "Really?", "Yes."], [1, 2, 3]),
        (['He', 'said', '"stop."', 'Then'], [3]),
        (["pi", "is", "3.14", "ok"], []),
    ],
)
def test_sentence_boundaries(words, cuts):
    assert sentence_boundaries(words) == cuts


# -------------------------------------------------------- strip_overlap --


def test_strip_overlap_removes_the_reheard_words():
    assert strip_overlap(["are", "you", "today."], ["You", "today.", "I", "am"]) == (["I", "am"], 2)


def test_strip_overlap_tolerates_one_leading_junk_word():
    assert strip_overlap(["you", "today."], ["ay", "you", "today.", "I"]) == (["I"], 3)


def test_strip_overlap_leaves_unrelated_text_alone():
    assert strip_overlap(["you", "today."], ["I", "am", "fine"]) == (["I", "am", "fine"], 0)
    assert strip_overlap([], ["a"]) == (["a"], 0)


# ------------------------------------------------------- LiveTranscript --


def test_live_transcript_cuts_committed_words_into_sentences():
    t = LiveTranscript()
    assert t.commit(["Hello", "there."]) [0].raw == "Hello there."
    assert t.commit(["How", "are"]) == []
    t.tentative = ["you"]
    done = t.commit(["you", "today?", "I"])
    assert [s.raw for s in done] == ["How are you today?"]
    t.tentative = ["am"]
    assert t.set_clean(0, "Hello there!")
    assert not t.set_clean(99, "nope")
    assert t.snapshot() == {
        "sentences": [
            {"id": 0, "raw": "Hello there.", "clean": "Hello there!"},
            {"id": 1, "raw": "How are you today?", "clean": None},
        ],
        "pending": "I",
        "tentative": "am",
    }
    assert t.raw_text() == "Hello there. How are you today? I am"
    assert t.final_text() == "Hello there! How are you today? I am"
    assert t.committed_words() == ["Hello", "there.", "How", "are", "you", "today?", "I"]


def test_live_transcript_close_pending():
    t = LiveTranscript()
    t.commit(["trailing", "words"])
    s = t.close_pending()
    assert s is not None and s.raw == "trailing words" and t.pending == []
    assert t.close_pending() is None


# ------------------------------------------------------ LiveTranscriber --


SCRIPT = [
    "hello there",
    "hello there how are",
    "hello there how are you today.",
    "hello there how are you today. I am",
    "hello there how are you today. I am fine.",
]


class Scripted:
    """Returns the next scripted hypothesis on every call (the last repeats)."""

    def __init__(self, script, fail_first: int = 0):
        self.script = list(script)
        self.calls = 0
        self.fail_first = fail_first
        self.lock = threading.Lock()

    def __call__(self, wav: bytes) -> str:
        with self.lock:
            self.calls += 1
            n = self.calls
        if n <= self.fail_first:
            raise RuntimeError("asr hiccup")
        index = min(n - 1 - self.fail_first, len(self.script) - 1)
        return self.script[index]


def fake_wav(audio, sample_rate) -> bytes:
    """Stand-in WAV builder: the raw int16 samples, no header."""
    return np.asarray(audio, dtype=np.int16).tobytes()


class Recorder:
    """Collects emitted events; thread-safe."""

    def __init__(self):
        self.events: list[dict] = []
        self.lock = threading.Lock()

    def __call__(self, event: dict) -> None:
        with self.lock:
            self.events.append(event)


def _wait(predicate, timeout: float = 5.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(0.005)
    return predicate()


def _scripted_run(cleanup_fn, *, script=SCRIPT, cleanup_enabled=True, fail_first=0,
                  deadline=2.5):
    fake = Scripted(script, fail_first=fail_first)
    events = Recorder()
    live = LiveTranscriber(
        transcribe_fn=fake,
        cleanup_fn=cleanup_fn,
        emit=events,
        sample_rate=16000,
        interval_s=0.05,
        cleanup_enabled=cleanup_enabled,
        cleanup_deadline_s=deadline,
        build_wav=fake_wav,
    )
    live.feed(np.zeros((8000, 1), dtype=np.int16))  # 0.5 s: never long enough to advance
    live.start()
    assert _wait(lambda: fake.calls >= len(script) + fail_first)
    text = live.finish()
    return live, text, events


def _committed(event: dict) -> str:
    snap = event["transcript"]
    parts = [s["raw"] for s in snap["sentences"]] + [snap["pending"]]
    return " ".join(p for p in parts if p)


def test_live_commits_in_order_and_cleans_every_sentence():
    calls: list[str] = []

    def cleanup(text: str) -> str:
        calls.append(text)
        return text[0].upper() + text[1:]

    live, text, events = _scripted_run(cleanup)

    # Committed text only ever grows, and each state extends the previous one.
    committed = [_committed(e) for e in events.events]
    for before, after in zip(committed, committed[1:]):
        assert after.startswith(before)
    seen = [c for i, c in enumerate(committed) if c and (i == 0 or c != committed[i - 1])]
    assert seen[0] == "hello there"
    assert "hello there how are" in seen
    assert "hello there how are you today." in seen

    # Every sentence is cleaned exactly once, in order (batched: the two
    # short sentences normally share one call).
    assert " ".join(calls) == "hello there how are you today. I am fine."
    assert text == "Hello there how are you today. I am fine."

    final = events.events[-1]
    assert final["type"] == "live" and final["final"] is True
    assert final["text"] == text
    assert [s["clean"] for s in final["transcript"]["sentences"]] == [
        "Hello there how are you today.", "I am fine."]
    assert all(e["type"] == "live" for e in events.events)


def test_live_tentative_is_reported_before_it_commits():
    live, _, events = _scripted_run(lambda t: t)
    tentatives = [e["transcript"]["tentative"] for e in events.events if not e.get("final")]
    assert "hello there" in tentatives  # first pass: nothing agreed yet


def test_live_cleanup_that_raises_loses_no_text():
    def boom(text: str) -> str:
        raise RuntimeError("ollama is down")

    _, text, _ = _scripted_run(boom)
    assert text == "hello there how are you today. I am fine."


def test_live_insane_cleanup_is_ignored():
    _, text, _ = _scripted_run(lambda t: "Sure! Here is a long answer to your question " * 3)
    assert text == "hello there how are you today. I am fine."


def test_live_slow_cleanup_falls_back_to_raw_at_the_deadline():
    def slow(text: str) -> str:
        time.sleep(1.0)
        return text.upper()

    t0 = time.time()
    _, text, _ = _scripted_run(slow, deadline=0.1)
    assert time.time() - t0 < 5
    # The first sentence may or may not have landed; the last one cannot have.
    assert text.endswith("I am fine.")
    assert text.lower() == "hello there how are you today. i am fine."


def test_live_raw_mode_never_calls_cleanup():
    calls: list[str] = []
    _, text, _ = _scripted_run(lambda t: calls.append(t) or t, cleanup_enabled=False)
    assert calls == []
    assert text == "hello there how are you today. I am fine."


def test_live_unfinished_remainder_is_cleaned_once():
    calls: list[str] = []

    def cleanup(text: str) -> str:
        calls.append(text)
        return text[0].upper() + text[1:]

    script = SCRIPT[:4] + ["hello there how are you today. I am fine"]
    _, text, _ = _scripted_run(cleanup, script=script)
    assert " ".join(calls) == "hello there how are you today. I am fine"
    assert calls[-1].endswith("I am fine")  # the remainder rides in the final flush
    assert text == "Hello there how are you today. I am fine"


def test_live_transcription_failures_just_retry():
    _, text, _ = _scripted_run(lambda t: t, fail_first=2)
    assert text == "hello there how are you today. I am fine."


def test_live_finish_survives_a_failing_final_pass():
    state = {"n": 0}

    def flaky(wav: bytes) -> str:
        state["n"] += 1
        if state["n"] > 2:
            raise RuntimeError("server went away")
        return SCRIPT[min(state["n"], 2)]  # passes 1-2: "...how are", "...today."

    live = LiveTranscriber(flaky, lambda t: t, lambda e: None, interval_s=0.05,
                           cleanup_enabled=False, build_wav=fake_wav)
    live.feed(np.zeros(8000, dtype=np.int16))
    live.start()
    assert _wait(lambda: state["n"] >= 3)
    # Committed "hello there how are", tentative "you today." -> both kept.
    assert live.finish() == "hello there how are you today."


def test_live_finish_with_no_audio_returns_empty():
    live = LiveTranscriber(lambda w: "never", lambda t: t, lambda e: None,
                           interval_s=0.05, build_wav=fake_wav)
    live.start()
    assert live.finish() == ""
    assert live.finish() == ""  # idempotent


def test_live_cancel_stops_without_transcribing():
    fake = Scripted(SCRIPT)
    live = LiveTranscriber(fake, lambda t: t, lambda e: None, interval_s=0.05,
                           build_wav=fake_wav)
    live.feed(np.zeros(8000, dtype=np.int16))
    live.cancel()
    live.start()
    time.sleep(0.2)
    assert fake.calls == 0


# ------------------------------------------- window advance (audio-aware) --


SR = 100          # samples per second in these tests
WORD = 50         # samples per word: 0.5 s of "speech"
SPEECH = ("Hello there. How are you today? I am fine thanks. "
          "The build is green. Ship it now.").split()


def word_audio(index: int) -> np.ndarray:
    return np.full((WORD, 1), index + 1, dtype=np.int16)


def audio_aware_transcribe(wav: bytes) -> str:
    """Return exactly the words whose samples are in the window, in order."""
    samples = np.frombuffer(wav, dtype=np.int16)
    ids: list[int] = []
    for value in samples:
        if value and (not ids or ids[-1] != value):
            ids.append(int(value))
    return " ".join(SPEECH[i - 1] for i in ids)


def test_live_window_advances_without_dropping_or_duplicating_words():
    cleaned: list[str] = []
    events = Recorder()
    live = LiveTranscriber(
        transcribe_fn=audio_aware_transcribe,
        cleanup_fn=lambda t: cleaned.append(t) or t,
        emit=events,
        sample_rate=SR,
        interval_s=0.02,
        overlap_s=1.0,          # two words of overlap
        min_window_s=0.1,
        build_wav=fake_wav,
    )
    live.start()
    for i in range(len(SPEECH)):
        live.feed(word_audio(i))
        time.sleep(0.08)        # several ticks per word
    text = live.finish()

    assert text == " ".join(SPEECH)
    assert live._offset > 0, "window never moved forward"
    # Every sentence cleaned exactly once, in order; batches end on sentence ends.
    assert " ".join(cleaned) == " ".join(SPEECH)
    assert all(c.endswith((".", "?")) for c in cleaned)
    assert len(cleaned) <= 5


def test_live_force_commits_when_the_window_exceeds_the_cap():
    words = [f"w{i}" for i in range(12)]   # no punctuation: never a sentence

    def transcribe(wav: bytes) -> str:
        samples = np.frombuffer(wav, dtype=np.int16)
        ids: list[int] = []
        for value in samples:
            if value and (not ids or ids[-1] != value):
                ids.append(int(value))
        return " ".join(words[i - 1] for i in ids)

    live = LiveTranscriber(transcribe, lambda t: t, lambda e: None, sample_rate=SR,
                           interval_s=0.02, cleanup_enabled=False, overlap_s=0.5,
                           max_window_s=2.0, min_window_s=0.1, build_wav=fake_wav)
    live.start()
    for i in range(len(words)):
        live.feed(np.full((WORD, 1), i + 1, dtype=np.int16))
        time.sleep(0.08)
    with live._state_lock:
        window = live._n_samples - live._offset
    text = live.finish()
    assert text == " ".join(words)
    assert live._offset > 0
    assert window <= 2.0 * SR + WORD * 3  # bounded, not the whole 6 s


# ------------------------------------------------ DictationEngine wiring --


from fethr.core import dictation as dictation_module  # noqa: E402
from fethr.core.dictation import DictationEngine, EventBus  # noqa: E402
from fethr.core.dictation import Recorder as MicRecorder  # noqa: E402
from fethr.core.settings import Settings  # noqa: E402


class FakeMic:
    """Stands in for :class:`fethr.core.dictation.Recorder`."""

    def __init__(self, wav: bytes | None = b"RIFF-full-clip"):
        self.on_frames = None
        self.wav = wav
        self.started = False

    def start(self) -> None:
        self.started = True

    def stop(self):
        return self.wav


def _engine(live: bool) -> DictationEngine:
    settings = Settings()
    settings.dictation.beeps = False
    settings.dictation.live_transcribe = live
    engine = DictationEngine(settings, EventBus())
    engine.warm_cleanup = lambda: False  # type: ignore[method-assign]  # no network in tests
    engine.injected = []  # type: ignore[attr-defined]
    engine.inject = engine.injected.append  # type: ignore[method-assign]
    return engine


def test_settings_live_transcribe_defaults_on_and_round_trips():
    assert Settings().dictation.live_transcribe is True
    s = Settings.from_dict({"dictation": {"live_transcribe": False}})
    assert s.dictation.live_transcribe is False
    assert Settings.from_dict(s.to_dict()).dictation.live_transcribe is False


def test_recorder_callback_forwards_frames_and_still_buffers():
    got: list = []
    mic = MicRecorder(on_frames=got.append)
    block = np.ones((4, 1), dtype=np.int16)
    mic._callback(block, 4, None, None)
    assert len(got) == 1 and np.array_equal(got[0], block)
    assert len(mic._frames) == 1
    mic.on_frames = lambda b: (_ for _ in ()).throw(RuntimeError("tap broke"))
    mic._callback(block, 4, None, None)  # a broken tap must not kill capture
    assert len(mic._frames) == 2


def test_engine_with_live_off_uses_the_whole_clip_path():
    engine = _engine(live=False)
    sent: list[bytes] = []
    engine.transcribe = lambda wav: sent.append(wav) or "hello world"  # type: ignore[method-assign]
    engine.cleanup = lambda text: "Hello world."  # type: ignore[method-assign]
    engine.recorder = FakeMic()  # type: ignore[assignment]
    engine.on_press("clean")
    assert engine._live is None and engine.recorder.on_frames is None
    engine.on_release("clean")
    assert _wait(lambda: engine.last_result["final"] == "Hello world.")
    assert sent == [b"RIFF-full-clip"]
    assert engine.injected == ["Hello world."]
    assert engine.last_result["raw"] == "hello world"


def test_engine_live_clean_mode_streams_and_pastes_the_cleaned_text():
    engine = _engine(live=True)
    wavs: list[bytes] = []

    def transcribe(wav: bytes) -> str:
        wavs.append(wav)
        return "hello there. how are you"

    cleaned: list[str] = []
    def cap_sentences(t: str) -> str:
        cleaned.append(t)
        return ". ".join(p[:1].upper() + p[1:] for p in t.split(". "))

    engine.transcribe = transcribe  # type: ignore[method-assign]
    engine.cleanup = cap_sentences  # type: ignore[method-assign]
    engine.recorder = FakeMic()  # type: ignore[assignment]
    events: list[dict] = []
    engine.bus.subscribe(events.append)

    engine.on_press("clean")
    live = engine._live
    assert live is not None and live.cleanup_enabled
    assert engine.recorder.on_frames == live.feed
    engine.recorder.on_frames(np.zeros((16000, 1), dtype=np.int16))
    engine.on_release("clean")

    assert _wait(lambda: engine.last_result["final"] != "")
    assert engine.injected == ["Hello there. How are you"]
    assert engine.last_result["raw"] == "hello there. how are you"
    assert " ".join(cleaned) == "hello there. how are you"
    assert b"RIFF-full-clip" not in wavs  # the whole-clip fallback was not needed
    assert _wait(lambda: any(e["type"] == "live" and e.get("final") for e in events))
    live_events = [e for e in events if e["type"] == "live"]
    assert all(e["mode"] == "clean" for e in live_events)


def test_engine_live_raw_mode_never_cleans():
    engine = _engine(live=True)
    engine.transcribe = lambda wav: "words as spoken"  # type: ignore[method-assign]
    engine.cleanup = lambda t: pytest.fail("raw mode must not clean")  # type: ignore[method-assign]
    engine.recorder = FakeMic()  # type: ignore[assignment]
    engine.on_press("raw")
    assert engine._live is not None and not engine._live.cleanup_enabled
    engine.recorder.on_frames(np.zeros((16000, 1), dtype=np.int16))
    engine.on_release("raw")
    assert _wait(lambda: engine.last_result["final"] == "words as spoken")


def test_engine_live_falls_back_to_the_whole_clip_when_live_is_empty():
    engine = _engine(live=True)
    sent: list[bytes] = []

    def transcribe(wav: bytes) -> str:
        sent.append(wav)
        return "fallback text" if wav == b"RIFF-full-clip" else ""

    engine.transcribe = transcribe  # type: ignore[method-assign]
    engine.recorder = FakeMic()  # type: ignore[assignment]
    engine.on_press("raw")
    engine.recorder.on_frames(np.zeros((16000, 1), dtype=np.int16))
    engine.on_release("raw")
    assert _wait(lambda: engine.last_result["final"] == "fallback text")
    assert sent[-1] == b"RIFF-full-clip"


def test_engine_live_too_short_cancels():
    engine = _engine(live=True)
    engine.transcribe = lambda wav: pytest.fail("nothing to transcribe")  # type: ignore[method-assign]
    engine.recorder = FakeMic(wav=None)  # type: ignore[assignment]
    engine.on_press("clean")
    live = engine._live
    engine.on_release("clean")
    assert live is not None and live._stop.is_set()
    assert engine._live is None
    assert engine.state.value == "idle"


def test_dictation_module_exposes_live_transcriber():
    assert dictation_module.LiveTranscriber is LiveTranscriber


# ------------------------------------------------ cleanup batching --------


@pytest.mark.parametrize(
    "cleaned, counts, parts",
    [
        ("Hi there. How are you?", [2, 3], ["Hi there.", "How are you?"]),
        ("Solo.", [1], ["Solo."]),
        # model split one sentence in two: cut at the end nearest the raw ratio
        ("A b. C d. E f g h.", [2, 6], ["A b.", "C d. E f g h."]),
        # model merged two sentences: proportional cut, nothing lost
        ("one two three four", [2, 2], ["one two", "three four"]),
        ("Yes, and no", [1, 1], ["Yes, and", "no"]),
        # fewer words than sentences: no split can work
        ("Yes", [1, 1], None),
        ("", [1], None),
        ("anything", [], []),
    ],
)
def test_split_cleaned(cleaned, counts, parts):
    assert split_cleaned(cleaned, counts) == parts


def test_split_cleaned_never_loses_or_blanks_words():
    text = "One two three. Four five, six seven eight nine ten."
    for counts in ([1, 1, 1], [5, 1, 1, 1], [3, 7], [2, 2, 2, 2, 2]):
        parts = split_cleaned(text, counts)
        assert parts is not None and len(parts) == len(counts)
        assert all(p.strip() for p in parts)
        assert " ".join(parts) == text


class FakeCleanup:
    """Records every call (text, context, start time); optionally slow/failing."""

    def __init__(self, fn=lambda t: t.upper(), delay: float = 0.0):
        self.fn = fn
        self.delay = delay
        self.calls: list[tuple[str, str, float]] = []
        self.lock = threading.Lock()

    def __call__(self, text: str, context: str = "") -> str:
        with self.lock:
            self.calls.append((text, context, time.monotonic()))
        if self.delay:
            time.sleep(self.delay)
        return self.fn(text)

    @property
    def texts(self) -> list[str]:
        return [c[0] for c in self.calls]


def _batching_live(cleanup, **kw) -> LiveTranscriber:
    """A transcriber whose cleanup worker runs but which never transcribes."""
    live = LiveTranscriber(lambda wav: "", cleanup, lambda e: None, interval_s=5.0,
                           build_wav=fake_wav, **kw)
    live.start()
    return live


def _say(live: LiveTranscriber, text: str) -> None:
    """Commit ``text`` as if agreement had just settled on it."""
    with live._state_lock:
        for sentence in live.transcript.commit(text.split()):
            live._queue_cleanup(sentence)


def test_batch_coalesces_short_sentences_that_arrive_inside_the_window():
    fake = FakeCleanup()
    live = _batching_live(fake, coalesce_s=0.5)
    _say(live, "Hi there.")
    time.sleep(0.1)
    _say(live, "Ok then.")
    assert _wait(lambda: fake.calls, 3)
    time.sleep(0.2)
    assert fake.texts == ["Hi there. Ok then."]  # one call, not two
    assert live.finish() == "HI THERE. OK THEN."
    snap = live.transcript.snapshot()
    assert [s["clean"] for s in snap["sentences"]] == ["HI THERE.", "OK THEN."]


def test_a_lone_short_sentence_is_sent_when_the_window_runs_out():
    fake = FakeCleanup()
    live = _batching_live(fake, coalesce_s=0.3)
    t0 = time.monotonic()
    _say(live, "Short one.")
    assert _wait(lambda: fake.calls, 3), "a lone short sentence must not wait forever"
    waited = fake.calls[0][2] - t0
    assert 0.2 <= waited < 1.5
    assert _wait(lambda: live.transcript.sentences[0].clean == "SHORT ONE.")
    live.cancel()


def test_batch_sends_at_once_when_big_enough():
    fake = FakeCleanup()
    live = _batching_live(fake, coalesce_s=3.0)
    t0 = time.monotonic()
    _say(live, "this single sentence is long enough to be worth a request on its own.")
    assert _wait(lambda: fake.calls, 2)
    assert fake.calls[0][2] - t0 < 1.0  # >= 12 words: no coalesce wait
    t1 = time.monotonic()
    _say(live, "One. Two.")  # two sentences at once: also no wait
    assert _wait(lambda: len(fake.calls) == 2, 2)
    assert fake.calls[1][2] - t1 < 1.0
    assert fake.texts[1] == "One. Two."
    live.cancel()


def test_finish_flushes_everything_uncleaned_in_one_call():
    fake = FakeCleanup()
    live = _batching_live(fake, coalesce_s=10.0)  # worker would wait a long time
    _say(live, "First bit. Second bit.")
    assert _wait(lambda: fake.calls, 2)  # 2 sentences -> sent at once
    _say(live, "Third bit.")
    _say(live, "and a tail")  # never finished: pending
    text = live.finish()
    assert fake.texts == ["First bit. Second bit.", "Third bit. and a tail"]
    assert text == "FIRST BIT. SECOND BIT. THIRD BIT. AND A TAIL"
    assert [s.clean for s in live.transcript.sentences] == [
        "FIRST BIT.", "SECOND BIT.", "THIRD BIT.", "AND A TAIL"]


def test_merged_reply_is_spread_over_its_sentences_without_blanks():
    # The model joined two sentences into one: no second sentence end.
    fake = FakeCleanup(fn=lambda t: "Hi there, how are you")
    live = _batching_live(fake, coalesce_s=10.0)
    _say(live, "hi there. how are you.")
    assert _wait(lambda: all(s.clean for s in live.transcript.sentences), 2)
    cleans = [s.clean for s in live.transcript.sentences]
    assert all(cleans) and " ".join(cleans) == "Hi there, how are you"
    assert live.finish() == "Hi there, how are you"


def test_failed_batch_falls_back_to_raw():
    def boom(text: str) -> str:
        raise RuntimeError("ollama is down")

    live = _batching_live(boom, coalesce_s=0.05)
    _say(live, "One thing. Another thing.")
    time.sleep(0.2)
    _say(live, "last")
    assert live.finish() == "One thing. Another thing. last"
    assert all(s.clean is None for s in live.transcript.sentences)


def test_unsplittable_batch_falls_back_to_raw():
    # A sane-sized reply that is too short to give each sentence a word.
    fake = FakeCleanup(fn=lambda t: "Yes")
    live = _batching_live(fake, coalesce_s=10.0)
    _say(live, "Yes. No.")
    assert live.finish() == "Yes. No."
    assert all(s.clean is None for s in live.transcript.sentences)


def test_context_carries_previous_final_words_and_is_capped():
    fake = FakeCleanup()
    live = _batching_live(fake, coalesce_s=10.0, context_words=4)
    _say(live, "alpha beta gamma delta epsilon.")  # 5 words, 1 sentence: waits
    _say(live, "zeta eta.")                         # 2nd sentence: batch goes now
    assert _wait(lambda: fake.calls, 2)
    assert fake.calls[0][1] == ""  # nothing came before the first batch
    assert _wait(lambda: live.transcript.sentences[1].clean is not None, 2)
    _say(live, "theta iota. kappa.")
    assert _wait(lambda: len(fake.calls) == 2, 2)
    # last 4 words of what is already final (cleaned where it landed)
    assert fake.calls[1][1] == "DELTA EPSILON. ZETA ETA."
    live.cancel()


def test_one_argument_cleanup_fn_still_works_with_context():
    got: list[str] = []
    live = _batching_live(lambda t: got.append(t) or t, coalesce_s=10.0)
    _say(live, "one. two.")
    assert _wait(lambda: got, 2)
    _say(live, "three. four.")
    assert _wait(lambda: len(got) == 2, 2)
    assert live.finish() == "one. two. three. four."


def test_raw_mode_batching_never_starts_a_cleaner():
    fake = FakeCleanup()
    live = _batching_live(fake, cleanup_enabled=False)
    _say(live, "One. Two. three")
    assert live.finish() == "One. Two. three"
    assert fake.calls == [] and live._cleaner is None
