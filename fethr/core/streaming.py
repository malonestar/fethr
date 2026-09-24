"""Live (streaming) transcription on top of a non-streaming whisper server.

Whisper transcribes a clip, not a stream, so live text is built the way
Wispr Flow and UFAL's ``whisper_streaming`` build it: every ``interval_s`` the
audio captured so far is transcribed again, and a word is *committed* once two
consecutive hypotheses agree on it (LocalAgreement-2).  Everything after the
agreed prefix is *tentative* and may still change on the next pass.

Committed text is cut into sentences.  Once a sentence is complete, the audio
window can move forward past it (keeping a second of overlap so the next pass
has context), which keeps each re-transcription short no matter how long the
key is held.  Completed sentences are handed to a second worker for the LLM
polish pass, so cleanup runs *while the user is still talking* and never
blocks transcription.

The polish pass is *batched*: when a sentence commits, the worker waits up to
``coalesce_s`` for more and sends everything queued as ONE request as soon as
it holds ``batch_min_sentences`` sentences or ``batch_min_words`` words — or
when the wait runs out, so a lone short sentence is never held back longer
than that.  Each request also carries the last ``context_words`` words that
are already final, so a correction or a pronoun that crosses a sentence
boundary is tidied with its antecedent in view.  The cleaned batch is split
back onto its sentences (:func:`split_cleaned`), so the per-sentence snapshot
the overlay renders keeps its shape.  At :meth:`LiveTranscriber.finish`
everything still uncleaned (including an unfinished remainder) goes in one
final request.

Layers
------
* Pure functions (:func:`tokenize`, :func:`local_agreement`,
  :func:`sentence_boundaries`, :func:`strip_overlap`) and the
  :class:`LiveTranscript` state object — no I/O, no threads.
* :class:`LiveTranscriber` — the threaded driver.  All I/O is injected
  (``transcribe_fn``, ``cleanup_fn``, ``emit``, ``build_wav``) so tests can run
  it end to end with fakes.
"""

from __future__ import annotations

import inspect
import io
import logging
import re
import threading
import time
import wave
from dataclasses import dataclass
from typing import Any, Callable

try:  # pragma: no cover - platform dependent
    import numpy as np
except ImportError:  # pragma: no cover
    np = None  # type: ignore[assignment]

log = logging.getLogger(__name__)

__all__ = [
    "LiveTranscriber",
    "LiveTranscript",
    "Sentence",
    "local_agreement",
    "pcm_wav",
    "sentence_boundaries",
    "split_cleaned",
    "strip_overlap",
    "tokenize",
]

#: Non-speech annotations whisper.cpp emits on silence or noise, e.g.
#: ``[BLANK_AUDIO]`` or ``[Music]``.  They are never dictated words.
_ANNOTATION = re.compile(r"\[[^\]]*\]")

#: Characters that may trail a sentence terminator (closing quotes/brackets).
_TRAILING_CLOSERS = "\"')]}”’»"


# --------------------------------------------------------------------------
# pure pieces
# --------------------------------------------------------------------------


def tokenize(text: str) -> list[str]:
    """Split a transcript into words, keeping punctuation attached.

    Bracketed whisper annotations such as ``[BLANK_AUDIO]`` are dropped.

    >>> tokenize("Hello, there.  How are you?")
    ['Hello,', 'there.', 'How', 'are', 'you?']
    >>> tokenize("[BLANK_AUDIO]")
    []
    """
    if not text:
        return []
    return _ANNOTATION.sub(" ", text).split()


def _norm(word: str) -> str:
    """Comparison key for a word: lower case, punctuation stripped.

    A word that is *all* punctuation (a stray dash) keeps its raw form so it
    still has to match exactly rather than matching everything.
    """
    key = re.sub(r"[^\w']", "", word.lower()).strip("'")
    return key or word


def local_agreement(
    prev_hyp: list[str], new_hyp: list[str], committed: list[str]
) -> tuple[list[str], list[str]]:
    """LocalAgreement-2: commit what the last two hypotheses agree on.

    Both hypotheses cover the same audio window and start with the words
    already committed from that window; those ``len(committed)`` words are
    skipped.  The longest common prefix of what remains (compared
    case- and punctuation-insensitively) is newly committed, spelled as in
    ``new_hyp``; the rest of ``new_hyp`` is tentative.

    Args:
        prev_hyp: the previous hypothesis for this window (``[]`` if none).
        new_hyp: the newest hypothesis for this window.
        committed: words of this window that are already committed.

    Returns:
        ``(newly_committed, tentative)``.

    >>> local_agreement(["hello", "there"], ["Hello", "there,", "how"], [])
    (['Hello', 'there,'], ['how'])
    """
    n = len(committed)
    prev = prev_hyp[n:]
    new = new_hyp[n:]
    k = 0
    while k < len(prev) and k < len(new) and _norm(prev[k]) == _norm(new[k]):
        k += 1
    return list(new[:k]), list(new[k:])


def _ends_sentence(word: str) -> bool:
    return word.rstrip(_TRAILING_CLOSERS).endswith((".", "?", "!"))


def sentence_boundaries(words: list[str]) -> list[int]:
    """Return the indexes just after each word that ends a sentence.

    >>> sentence_boundaries(["Hi.", "How", "are", "you?", "Fine"])
    [1, 4]
    """
    return [i + 1 for i, word in enumerate(words) if _ends_sentence(word)]


def strip_overlap(tail: list[str], hyp: list[str], max_n: int = 8) -> tuple[list[str], int]:
    """Remove the re-heard overlap from the front of a hypothesis.

    After the audio window moves forward it deliberately keeps ~1 s of audio
    that was already committed, so each new hypothesis starts by repeating
    the last few committed words.  Find the longest suffix of ``tail`` (the
    committed words just before the cut) that ``hyp`` starts with — allowing
    one leading junk word, e.g. half a word cut by the window edge — and drop
    it.

    Returns:
        ``(hyp_without_overlap, words_removed)``.

    >>> strip_overlap(["you", "today."], ["You", "today.", "I", "am"])
    (['I', 'am'], 2)
    """
    if not tail or not hyp:
        return list(hyp), 0
    tail_keys = [_norm(w) for w in tail]
    hyp_keys = [_norm(w) for w in hyp]
    for n in range(min(len(tail), max_n, len(hyp)), 0, -1):
        for skip in (0, 1):
            if skip and n < 2:
                continue  # a one-word match after junk is too weak to trust
            if skip + n > len(hyp):
                continue
            if hyp_keys[skip:skip + n] == tail_keys[-n:]:
                return list(hyp[skip + n:]), skip + n
    return list(hyp), 0


def split_cleaned(cleaned: str, raw_counts: list[int]) -> list[str] | None:
    """Split one cleaned batch back onto the sentences it was made from.

    ``raw_counts`` is the raw word count of each sentence in the batch.  When
    the cleaned text has at least as many sentence ends as the batch had
    sentences, cuts are taken at sentence ends — the ones nearest where the
    raw word counts say each sentence should end.  When the model merged
    sentences (fewer ends), the words are cut proportionally instead.  Either
    way every word lands in exactly one piece, in order, so joining the
    pieces with spaces gives back ``cleaned`` (whitespace-normalised) and no
    sentence is left blank for the overlay to fall back to raw on.

    Returns ``None`` when there are fewer cleaned words than sentences (no
    split can give every sentence something); the caller keeps raw.

    >>> split_cleaned("Hi there. How are you?", [2, 3])
    ['Hi there.', 'How are you?']
    >>> split_cleaned("One two three four", [2, 2])
    ['One two', 'three four']
    """
    n = len(raw_counts)
    tokens = cleaned.split()
    if n == 0:
        return []
    if not tokens or len(tokens) < n:
        return None
    if n == 1:
        return [" ".join(tokens)]
    total_raw = sum(raw_counts) or n
    targets: list[float] = []
    acc = 0
    for count in raw_counts[:-1]:
        acc += count
        targets.append(acc / total_raw * len(tokens))
    bounds = [b for b in sentence_boundaries(tokens) if 0 < b < len(tokens)]
    cuts: list[int] = []
    if len(bounds) >= n - 1:
        lo = 0
        for i, target in enumerate(targets):
            hi = len(bounds) - (n - 1 - i)  # leave enough ends for the rest
            best = min(range(lo, hi + 1), key=lambda j: abs(bounds[j] - target))
            cuts.append(bounds[best])
            lo = best + 1
    else:
        prev = 0
        for i, target in enumerate(targets):
            cut = max(prev + 1, min(int(round(target)), len(tokens) - (n - 1 - i)))
            cuts.append(cut)
            prev = cut
    edges = [0] + cuts + [len(tokens)]
    return [" ".join(tokens[a:b]) for a, b in zip(edges, edges[1:])]


def _accepts_context(fn: Callable[..., Any]) -> bool:
    """True when ``fn`` can be called with a ``context=`` keyword."""
    try:
        params = inspect.signature(fn).parameters.values()
    except (TypeError, ValueError):
        return False
    return any(p.name == "context" or p.kind is inspect.Parameter.VAR_KEYWORD for p in params)


def pcm_wav(audio: Any, sample_rate: int) -> bytes:
    """Encode an int16 mono sample array as a WAV file."""
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(audio.astype("int16").tobytes())
    return buf.getvalue()


# --------------------------------------------------------------------------
# transcript state
# --------------------------------------------------------------------------


@dataclass
class Sentence:
    """One committed sentence and (once the polish pass lands) its cleanup."""

    id: int
    raw: str
    clean: str | None = None

    def as_dict(self) -> dict[str, Any]:
        """JSON-friendly view."""
        return {"id": self.id, "raw": self.raw, "clean": self.clean}


class LiveTranscript:
    """Committed sentences + committed-but-unfinished words + tentative words.

    Not thread-safe on its own; :class:`LiveTranscriber` guards it.
    """

    def __init__(self) -> None:
        self.sentences: list[Sentence] = []
        self.pending: list[str] = []
        self.tentative: list[str] = []
        self._next_id = 0

    def commit(self, words: list[str]) -> list[Sentence]:
        """Append committed words; return any sentences they complete."""
        self.pending.extend(words)
        done: list[Sentence] = []
        start = 0
        for cut in sentence_boundaries(self.pending):
            done.append(self._new_sentence(" ".join(self.pending[start:cut])))
            start = cut
        self.pending = self.pending[start:]
        return done

    def close_pending(self) -> Sentence | None:
        """Turn the unfinished remainder into a sentence of its own."""
        if not self.pending:
            return None
        sentence = self._new_sentence(" ".join(self.pending))
        self.pending = []
        return sentence

    def _new_sentence(self, raw: str) -> Sentence:
        sentence = Sentence(self._next_id, raw)
        self._next_id += 1
        self.sentences.append(sentence)
        return sentence

    def set_clean(self, sentence_id: int, clean: str) -> bool:
        """Record the cleaned text of sentence ``sentence_id``."""
        for sentence in self.sentences:
            if sentence.id == sentence_id:
                sentence.clean = clean
                return True
        return False

    def committed_words(self) -> list[str]:
        """Every committed word, in order."""
        words: list[str] = []
        for sentence in self.sentences:
            words.extend(sentence.raw.split())
        words.extend(self.pending)
        return words

    def raw_text(self) -> str:
        """Committed + tentative text exactly as whisper produced it."""
        parts = [s.raw for s in self.sentences] + self.pending + self.tentative
        return " ".join(p for p in parts if p)

    def final_text(self) -> str:
        """Clean-or-raw per sentence, then any remainder, space-joined."""
        parts = [s.clean or s.raw for s in self.sentences] + self.pending + self.tentative
        return " ".join(p for p in parts if p)

    def snapshot(self) -> dict[str, Any]:
        """JSON-friendly view for the UI."""
        return {
            "sentences": [s.as_dict() for s in self.sentences],
            "pending": " ".join(self.pending),
            "tentative": " ".join(self.tentative),
        }


# --------------------------------------------------------------------------
# threaded driver
# --------------------------------------------------------------------------


class LiveTranscriber:
    """Re-transcribes growing audio on a timer and commits stable text.

    Args:
        transcribe_fn: ``wav_bytes -> text``; may raise (the window is retried
            on the next tick).
        cleanup_fn: ``text -> cleaned text or None``; may raise.  Results that
            fail :func:`~fethr.core.dictation.cleanup_looks_sane` are ignored.
            If it accepts a ``context`` keyword it is passed the words that
            are already final before the batch (never to be re-output).
        emit: receives ``{"type": "live", ...}`` events.  Must not block.
        sample_rate: of the frames passed to :meth:`feed`.
        interval_s: seconds between re-transcriptions.
        cleanup_enabled: run ``cleanup_fn`` per completed sentence.
        overlap_s: audio kept before a committed sentence boundary when the
            window moves forward, so the next pass hears context.
        max_window_s: longest window ever sent (whisper's native 30 s).
        min_window_s: windows shorter than this are not worth a request.
        cleanup_deadline_s: how long :meth:`finish` waits for an in-flight
            cleanup batch, and then again for the final flush batch, before
            falling back to raw for whatever has not landed.
        finish_wait_s: how long :meth:`finish` waits for an in-flight tick.
        coalesce_s: after a sentence commits, how long the cleanup worker
            waits for more before sending what it has.
        batch_min_sentences / batch_min_words: send at once (no more
            waiting) when the queue holds this many sentences or words.
        context_words: how many already-final words ride along as context.
        build_wav: ``(int16 array, sample_rate) -> bytes``; injectable for tests.
    """

    def __init__(
        self,
        transcribe_fn: Callable[[bytes], str],
        cleanup_fn: Callable[[str], str | None],
        emit: Callable[[dict[str, Any]], None],
        sample_rate: int = 16000,
        interval_s: float = 1.5,
        cleanup_enabled: bool = True,
        *,
        overlap_s: float = 1.0,
        max_window_s: float = 30.0,
        min_window_s: float = 0.3,
        cleanup_deadline_s: float = 2.5,
        finish_wait_s: float = 35.0,
        build_wav: Callable[[Any, int], bytes] = pcm_wav,
        coalesce_s: float = 0.4,
        batch_min_sentences: int = 2,
        batch_min_words: int = 12,
        context_words: int = 30,
    ) -> None:
        self.transcribe_fn = transcribe_fn
        self.cleanup_fn = cleanup_fn
        self.emit = emit
        self.sample_rate = sample_rate
        self.interval_s = interval_s
        self.cleanup_enabled = cleanup_enabled
        self.overlap_s = overlap_s
        self.max_window_s = max_window_s
        self.min_window_s = min_window_s
        self.cleanup_deadline_s = cleanup_deadline_s
        self.finish_wait_s = finish_wait_s
        self.build_wav = build_wav
        self.coalesce_s = coalesce_s
        self.batch_min_sentences = batch_min_sentences
        self.batch_min_words = batch_min_words
        self.context_words = context_words
        self._cleanup_takes_context = _accepts_context(cleanup_fn)

        self.transcript = LiveTranscript()
        #: Transcription passes that returned (successfully), for diagnostics.
        self.passes = 0
        #: ``cleanup_fn`` calls made (one per batch), for diagnostics.
        self.cleanup_calls = 0

        # audio (feed() side)
        self._chunks: list[Any] = []
        self._n_samples = 0
        self._audio_lock = threading.Lock()

        # window state (guarded by _state_lock)
        self._state_lock = threading.RLock()
        self._offset = 0                 # first sample of the current window
        self._prev_hyp: list[str] = []   # previous hypothesis, overlap stripped
        self._win_committed: list[str] = []  # committed words of this window
        self._overlap_tail: list[str] = []   # committed words heard again at window start
        self._last_emitted: dict[str, Any] | None = None
        self._closed = False

        # threads
        self._tick_lock = threading.Lock()
        self._stop = threading.Event()
        self._worker: threading.Thread | None = None
        self._cleaner: threading.Thread | None = None
        # cleanup batching (guarded by _cleanup_cond)
        self._cleanup_cond = threading.Condition()
        self._uncleaned: list[Sentence] = []  # committed, not yet sent
        self._inflight = 0                    # batches the worker is cleaning
        self._cleaner_done = False            # worker must take no more batches
        self._final_text: str | None = None

    # -- lifecycle -------------------------------------------------------

    def start(self) -> None:
        """Start the transcription timer and the cleanup worker."""
        if self._worker is not None:
            return
        self._worker = threading.Thread(target=self._run, name="fethr-live", daemon=True)
        self._worker.start()
        if self.cleanup_enabled:
            self._cleaner = threading.Thread(
                target=self._clean_loop, name="fethr-live-cleanup", daemon=True
            )
            self._cleaner.start()

    def feed(self, frames: Any) -> None:
        """Append captured int16 frames.  O(1): called from the audio callback."""
        with self._audio_lock:
            self._chunks.append(frames)
            self._n_samples += len(frames)

    def cancel(self) -> None:
        """Abandon this utterance: stop both workers, transcribe nothing more."""
        self._stop.set()
        with self._state_lock:
            self._closed = True
        self._stop_cleaner()

    def _stop_cleaner(self) -> None:
        with self._cleanup_cond:
            self._cleaner_done = True
            self._cleanup_cond.notify_all()

    def finish(self) -> str:
        """Stop, transcribe the tail, commit everything and return the text.

        Never raises; on any failure it returns the best text it has (which
        may be ``""`` — the caller then falls back to a whole-clip pass).
        """
        if self._final_text is not None:
            return self._final_text
        self._stop.set()
        try:
            text = self._finish()
        except Exception:
            log.exception("live finish failed; returning what was committed")
            with self._state_lock:
                text = self.transcript.final_text()
        self._final_text = text
        return text

    @property
    def raw_text(self) -> str:
        """Everything transcribed so far, before any cleanup."""
        with self._state_lock:
            return self.transcript.raw_text()

    # -- audio -----------------------------------------------------------

    def _audio_from(self, start: int) -> Any:
        """Return samples ``[start:]`` as one flat int16 array."""
        with self._audio_lock:
            if not self._chunks:
                return np.zeros(0, dtype="int16")
            if len(self._chunks) > 1:
                self._chunks = [np.concatenate([np.asarray(c).reshape(-1) for c in self._chunks])]
            audio = np.asarray(self._chunks[0]).reshape(-1)
        return audio[start:]

    # -- transcription loop ----------------------------------------------

    def _run(self) -> None:
        while not self._stop.wait(self.interval_s):
            try:
                with self._tick_lock:
                    if self._stop.is_set():
                        return
                    self._tick()
            except Exception:
                log.exception("live tick failed")

    def _tick(self) -> None:
        with self._state_lock:
            offset = self._offset
        audio = self._audio_from(offset)
        if len(audio) < self.min_window_s * self.sample_rate:
            return
        try:
            text = self.transcribe_fn(self.build_wav(audio, self.sample_rate))
        except Exception as exc:
            log.warning("live transcription failed (retrying next tick): %s", exc)
            return
        self.passes += 1
        with self._state_lock:
            if self._closed or offset != self._offset:
                return
            hyp, stripped = strip_overlap(self._overlap_tail, tokenize(text))
            self._apply(hyp, stripped, len(audio))
        self._emit_snapshot()

    def _apply(self, hyp: list[str], stripped: int, window_len: int) -> None:
        """Run agreement on ``hyp`` and update state.  Holds ``_state_lock``."""
        newly, tentative = local_agreement(self._prev_hyp, hyp, self._win_committed)
        self._prev_hyp = hyp
        forced = False
        if window_len > self.max_window_s * self.sample_rate:
            # Whisper will not see past 30 s: commit the oldest words now
            # (holding the last two back, in case they are cut mid-word, when
            # there are more than two) and move the window regardless.
            force = tentative[:-2] if len(tentative) > 2 else tentative
            newly = newly + force
            tentative = tentative[len(force):]
            forced = True
        self._win_committed.extend(newly)
        done = self.transcript.commit(newly)
        self.transcript.tentative = tentative
        for sentence in done:
            self._queue_cleanup(sentence)
        if done or forced:
            self._advance(hyp, stripped, window_len, forced)

    def _advance(self, hyp: list[str], stripped: int, window_len: int, forced: bool) -> None:
        """Move the window start past committed text.  Holds ``_state_lock``.

        The cut point is a proportional estimate: if the boundary is word
        ``b`` of ``n`` heard in the window, it sits at ``b/n`` of the audio.
        ``overlap_s`` of audio is kept before it so a word straddling the
        estimate is heard whole again next pass (and stripped by
        :func:`strip_overlap`).
        """
        carried = [] if forced else list(self.transcript.pending)
        boundary = stripped + len(self._win_committed) - len(carried)
        total = stripped + len(hyp)
        if boundary <= 0 or total <= 0:
            return
        estimate = int(window_len * min(1.0, boundary / total))
        new_offset = self._offset + estimate - int(self.overlap_s * self.sample_rate)
        if new_offset <= self._offset:
            return
        committed = self.transcript.committed_words()
        before_cut = committed[: len(committed) - len(carried)] if carried else committed
        self._overlap_tail = before_cut[-8:]
        self._offset = new_offset
        self._win_committed = carried
        # What we already believe about the new window, so the next pass can
        # agree with it instead of starting from nothing.
        self._prev_hyp = carried + list(self.transcript.tentative)
        log.debug("live window advanced to %.2fs", new_offset / self.sample_rate)

    # -- cleanup worker --------------------------------------------------

    def _queue_cleanup(self, sentence: Sentence) -> None:
        if not self.cleanup_enabled:
            return
        with self._cleanup_cond:
            self._uncleaned.append(sentence)
            self._cleanup_cond.notify_all()

    def _batch_ready(self, batch: list[Sentence]) -> bool:
        """Big enough to send without waiting for more."""
        if len(batch) >= self.batch_min_sentences:
            return True
        return sum(len(s.raw.split()) for s in batch) >= self.batch_min_words

    def _context_before(self, first_id: int) -> str:
        """The last ``context_words`` final words before sentence ``first_id``.

        Holds ``_state_lock``.  Cleaned text where it has landed, raw where
        it has not — either way it is text the model must not re-output.
        """
        words: list[str] = []
        for sentence in self.transcript.sentences:
            if sentence.id >= first_id:
                break
            words.extend((sentence.clean if sentence.clean is not None else sentence.raw).split())
        return " ".join(words[-self.context_words:]) if self.context_words > 0 else ""

    def _clean_one(self, raw: str, context: str = "") -> str | None:
        """Run ``cleanup_fn`` on ``raw``; None unless the result is sane.

        The sanity guard compares against ``raw`` only — the context is
        never part of what the model was asked to output.
        """
        from .dictation import cleanup_looks_sane  # lazy: dictation imports us

        try:
            self.cleanup_calls += 1
            if context and self._cleanup_takes_context:
                cleaned = self.cleanup_fn(raw, context=context)  # type: ignore[call-arg]
            else:
                cleaned = self.cleanup_fn(raw)
        except Exception as exc:
            log.warning("live cleanup failed; keeping raw: %s", exc)
            return None
        if cleaned is None:
            return None
        cleaned = cleaned.strip()
        return cleaned if cleanup_looks_sane(raw, cleaned) else None

    def _clean_batch(self, batch: list[Sentence]) -> None:
        """Clean ``batch`` in one call and spread the result over its sentences.

        Any failure (call raised, reply insane, no workable split) leaves
        every sentence of the batch raw — never a partial or blank result.
        """
        if not batch:
            return
        raw = " ".join(s.raw for s in batch)
        with self._state_lock:
            context = self._context_before(batch[0].id)
        cleaned = self._clean_one(raw, context)
        if cleaned is None:
            return
        parts = split_cleaned(cleaned, [len(s.raw.split()) for s in batch])
        if parts is None:
            log.info("live cleanup could not be split over %d sentences; keeping raw", len(batch))
            return
        with self._state_lock:
            if self._closed:
                return
            for sentence, part in zip(batch, parts):
                sentence.clean = part
        self._emit_snapshot()

    def _clean_loop(self) -> None:
        cond = self._cleanup_cond
        while True:
            with cond:
                cond.wait_for(lambda: self._uncleaned or self._cleaner_done)
                if self._cleaner_done:
                    return
                # Coalesce: give more sentences a moment to arrive, but send
                # as soon as the batch is worth a call on its own.
                deadline = time.monotonic() + self.coalesce_s
                while not self._cleaner_done and not self._batch_ready(self._uncleaned):
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        break
                    cond.wait(remaining)
                if self._cleaner_done:
                    return  # finish() takes whatever is still queued
                batch, self._uncleaned = self._uncleaned, []
                self._inflight += 1
            try:
                self._clean_batch(batch)
            except Exception:
                log.exception("live cleanup batch failed; keeping raw")
            finally:
                with cond:
                    self._inflight -= 1
                    cond.notify_all()

    # -- finish ----------------------------------------------------------

    def _finish(self) -> str:
        got = self._tick_lock.acquire(timeout=self.finish_wait_s)
        try:
            with self._state_lock:
                offset = self._offset
            hyp: list[str] | None = None
            audio = self._audio_from(offset) if got else None
            if audio is not None and len(audio) >= 0.1 * self.sample_rate:
                try:
                    text = self.transcribe_fn(self.build_wav(audio, self.sample_rate))
                    self.passes += 1
                    with self._state_lock:
                        hyp, _ = strip_overlap(self._overlap_tail, tokenize(text))
                except Exception as exc:
                    log.warning("final live transcription failed; using last hypothesis: %s", exc)
            with self._state_lock:
                latest = self._prev_hyp
                if hyp is None or (len(hyp) <= len(self._win_committed) < len(latest)):
                    hyp = latest
                rest = hyp[len(self._win_committed):]
                self._win_committed.extend(rest)
                self.transcript.tentative = []
                for sentence in self.transcript.commit(rest):
                    self._queue_cleanup(sentence)
        finally:
            if got:
                self._tick_lock.release()

        # The worker takes no new batch from here on; everything it has not
        # taken is flushed below in ONE call.
        self._stop_cleaner()
        if self.cleanup_enabled:
            with self._cleanup_cond:
                self._cleanup_cond.wait_for(
                    lambda: self._inflight <= 0, timeout=self.cleanup_deadline_s
                )

        with self._state_lock:
            remainder = self.transcript.close_pending()
        if self.cleanup_enabled:
            with self._cleanup_cond:
                batch, self._uncleaned = self._uncleaned, []
            if remainder is not None:
                batch.append(remainder)
            if batch:
                self._flush(batch)

        with self._state_lock:
            self._closed = True
            final = self.transcript.final_text()
            snapshot = self.transcript.snapshot()
        self._safe_emit({"type": "live", "final": True, "transcript": snapshot, "text": final})
        return final

    def _flush(self, batch: list[Sentence]) -> None:
        """Clean the final batch, waiting at most ``cleanup_deadline_s``.

        Runs on a helper thread so a hung server cannot hold the paste; a
        result landing after the deadline is dropped (``_closed`` is set).
        """
        done = threading.Event()

        def run() -> None:
            try:
                self._clean_batch(batch)
            except Exception:
                log.exception("final cleanup batch failed; keeping raw")
            finally:
                done.set()

        threading.Thread(target=run, name="fethr-live-flush", daemon=True).start()
        if not done.wait(self.cleanup_deadline_s):
            log.info("final cleanup missed the %.1fs deadline; keeping raw", self.cleanup_deadline_s)

    # -- events ----------------------------------------------------------

    def _emit_snapshot(self) -> None:
        with self._state_lock:
            if self._closed:
                return
            snapshot = self.transcript.snapshot()
            if snapshot == self._last_emitted:
                return
            self._last_emitted = snapshot
        self._safe_emit({"type": "live", "final": False, "transcript": snapshot})

    def _safe_emit(self, event: dict[str, Any]) -> None:
        try:
            self.emit(event)
        except Exception:
            log.debug("live emit failed", exc_info=True)
