"""Benchmark the live cleanup pass: per-sentence (old) vs batched (new).

Replays a scripted ~120-word dictation through :class:`LiveTranscriber` at a
realistic speaking pace (whisper is faked: every 1.5 s tick "hears" four more
words), with the REAL Ollama cleanup model behind it, and prints for each mode:
cleanup calls, time spent in cleanup calls, how long ``finish()`` took after
key-up (what the user waits for), sentences that fell back to raw, and the
final text.

* old = one ``/api/chat`` call per committed sentence, no context
  (the pre-batching behaviour, reproduced by a wrapper that splits every
  batch back into sentences and cleans them one at a time);
* new = coalesced batches with the previous ~30 final words as context
  (default 0.4 s coalesce window);
* wide = the same, with the coalesce window stretched past one tick
  (``INTERVAL_S + 0.1``) so consecutive sentences can share a call.

Both modes use the same request options (keep_alive 10m, num_predict cap) and
both are preceded by one warm-up, so the difference is the batching/context.

Usage: python cleanup_bench.py [ollama_url] [model] [--words-per-tick N]
                                [--modes old,new,wide]

``--words-per-tick`` raises the burstiness: 4 is a steady ~160 wpm speaker
with whisper keeping up; 12+ models a fast speaker or a lagging ASR, where
several sentences commit in one tick.
"""
from __future__ import annotations

import argparse
import threading
import time

import numpy as np

from fethr.core.dictation import DictationEngine
from fethr.core.settings import Settings
from fethr.core.streaming import LiveTranscriber, sentence_boundaries

_ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
_ap.add_argument("url", nargs="?", default="http://100.101.145.5:11434")
_ap.add_argument("model", nargs="?", default="hf.co/Qwen/Qwen3-14B-GGUF:Q5_K_M")
_ap.add_argument("--words-per-tick", type=int, default=4)
_ap.add_argument("--modes", default="old,new,wide")
ARGS = _ap.parse_args()
URL, MODEL = ARGS.url, ARGS.model

SCRIPT = (
    "So I spent the morning on the sidecar firmware. Um the chain probe kept "
    "timing out on the second port. I think it was the UART pin. No wait, it "
    "was the TX pin being driven during the re-begin. Anyway I added a GPIO "
    "reset before every begin and it came back. The companion display works "
    "now too. It shows the layer letter and uh a little spinner. Next I want "
    "to make the key swap a runtime setting. That way nobody has to reflash "
    "just to flip the keys. Can you remind me to update the flash doc "
    "tomorrow? Also the release notes need the new protocol commands, you "
    "know, the swap and orientation ones. Let's ship it on Friday, actually "
    "no, Thursday afternoon."
)
WORDS = SCRIPT.split()
WORDS_PER_TICK = ARGS.words_per_tick
INTERVAL_S = 1.5


class Timed:
    """Wraps a cleanup callable and records wall time per call."""

    def __init__(self, fn):
        self.fn = fn
        self.durations: list[float] = []
        self.lock = threading.Lock()

    def timed(self, *args, **kwargs):
        t0 = time.perf_counter()
        try:
            return self.fn(*args, **kwargs)
        finally:
            with self.lock:
                self.durations.append(time.perf_counter() - t0)


def run(mode: str, engine: DictationEngine) -> dict:
    calls = {"n": 0}

    def transcribe(_wav: bytes) -> str:
        calls["n"] += 1
        return " ".join(WORDS[: min(calls["n"] * WORDS_PER_TICK, len(WORDS))])

    if mode == "old":
        timer = Timed(engine.cleanup)

        def per_sentence(text: str) -> str:
            # Old behaviour: every sentence is its own request, no context.
            words = text.split()
            cuts = [0] + sentence_boundaries(words)
            if cuts[-1] != len(words):
                cuts.append(len(words))
            parts = [" ".join(words[a:b]) for a, b in zip(cuts, cuts[1:])]
            return " ".join(timer.timed(p) for p in parts)

        cleanup_fn = per_sentence
        live_kwargs = {"coalesce_s": 0.0, "batch_min_sentences": 1, "context_words": 0}
    else:
        timer = Timed(engine.cleanup)

        def batched(text: str, context: str = "") -> str:
            return timer.timed(text, context=context)

        cleanup_fn = batched
        live_kwargs = {} if mode == "new" else {"coalesce_s": INTERVAL_S + 0.1}

    live = LiveTranscriber(
        transcribe_fn=transcribe,
        cleanup_fn=cleanup_fn,
        emit=lambda e: None,
        sample_rate=16000,
        interval_s=INTERVAL_S,
        cleanup_enabled=True,
        overlap_s=1e6,        # fake audio: never move the window
        max_window_s=1e9,
        build_wav=lambda audio, sr: b"",
        **live_kwargs,
    )
    live.feed(np.zeros((16000, 1), dtype=np.int16))
    t_start = time.perf_counter()
    live.start()
    ticks_needed = -(-len(WORDS) // WORDS_PER_TICK) + 1  # all words heard twice
    while calls["n"] < ticks_needed:
        time.sleep(0.05)
    t_release = time.perf_counter()
    text = live.finish()
    t_done = time.perf_counter()
    raw_left = sum(1 for s in live.transcript.sentences if s.clean is None)
    return {
        "mode": mode,
        "calls": len(timer.durations),
        "cleanup_s": sum(timer.durations),
        "max_call_s": max(timer.durations, default=0.0),
        "finish_s": t_done - t_release,
        "total_s": t_done - t_start,
        "sentences": len(live.transcript.sentences),
        "raw_fallbacks": raw_left,
        "text": text,
    }


def main() -> None:
    settings = Settings()
    settings.dictation.cleanup_url = URL
    settings.dictation.cleanup_model = MODEL
    settings.dictation.cleanup_enabled = True
    settings.dictation.cleanup_timeout = 60
    engine = DictationEngine(settings)
    print(f"cleanup: {URL} {MODEL}")
    print(f"script: {len(WORDS)} words, {len(sentence_boundaries(WORDS))} sentences, "
          f"{WORDS_PER_TICK} words per {INTERVAL_S}s tick")

    results = []
    for mode in ARGS.modes.split(","):
        t0 = time.perf_counter()
        engine._warmup_request()  # same warm start for both modes
        print(f"\n[{mode}] warm-up {time.perf_counter() - t0:.2f}s, running ...", flush=True)
        results.append(run(mode, engine))

    print()
    print(f"{'mode':<5} {'calls':>5} {'cleanup s':>10} {'max call s':>11} "
          f"{'finish s':>9} {'total s':>8} {'raw left':>9}")
    for r in results:
        print(f"{r['mode']:<5} {r['calls']:>5} {r['cleanup_s']:>10.2f} {r['max_call_s']:>11.2f} "
              f"{r['finish_s']:>9.2f} {r['total_s']:>8.2f} {r['raw_fallbacks']:>4}/{r['sentences']}")
    for r in results:
        print(f"\n--- {r['mode']} final text ---\n{r['text']}")


if __name__ == "__main__":
    main()
