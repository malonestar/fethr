"""Talk into the real mic and watch live transcription commit, sentence by sentence.

Opens the default (or ``--device``) microphone for N seconds, feeds it to a
:class:`fethr.core.streaming.LiveTranscriber` wired to the real whisper.cpp
and Ollama servers, prints every ``live`` snapshot as it arrives, then the
final text with timings.  Nothing is pasted.

Usage:
    python live_probe.py [asr_url] [cleanup_url] [--seconds 12] [--mode clean|raw]
                         [--model qwen3:14b] [--interval 1.5] [--device N]

Defaults come from fethr's saved settings.
"""
from __future__ import annotations

import argparse
import sys
import time

import sounddevice as sd

from fethr.core.dictation import DictationEngine, EventBus
from fethr.core.settings import load_settings
from fethr.core.streaming import LiveTranscriber


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("asr_url", nargs="?", help="whisper.cpp server, e.g. http://host:8890")
    parser.add_argument("cleanup_url", nargs="?", help="Ollama server, e.g. http://host:11434")
    parser.add_argument("--seconds", type=float, default=12.0)
    parser.add_argument("--mode", choices=("clean", "raw"), default="clean")
    parser.add_argument("--model", help="cleanup model (default: from settings)")
    parser.add_argument("--interval", type=float, default=1.5)
    parser.add_argument("--device", help="input device index or name substring")
    args = parser.parse_args()

    settings = load_settings()
    cfg = settings.dictation
    if args.asr_url:
        cfg.asr_url = args.asr_url
    if args.cleanup_url:
        cfg.cleanup_url = args.cleanup_url
    if args.model:
        cfg.cleanup_model = args.model
    cleanup_on = args.mode == "clean"
    cfg.cleanup_enabled = cleanup_on
    device = args.device
    if device is not None and device.isdigit():
        device = int(device)
    elif device is None:
        device = settings.audio.input_device

    # The engine is only borrowed for its transcribe()/cleanup() network calls.
    engine = DictationEngine(settings, EventBus())
    t0 = time.time()
    last = {"line": ""}

    def show(event: dict) -> None:
        snap = event["transcript"]
        done = " ".join(
            (f"[{s['clean']}]" if s["clean"] else s["raw"]) for s in snap["sentences"]
        )
        line = " | ".join(p for p in (done, snap["pending"], f"~{snap['tentative']}~"
                                      if snap["tentative"] else "") if p)
        if line != last["line"] or event.get("final"):
            tag = "FINAL" if event.get("final") else "live "
            print(f"{time.time() - t0:6.2f}s {tag} {line}", flush=True)
            last["line"] = line

    live = LiveTranscriber(
        transcribe_fn=engine.transcribe,
        cleanup_fn=engine.cleanup,
        emit=show,
        sample_rate=cfg.sample_rate,
        interval_s=args.interval,
        cleanup_enabled=cleanup_on,
    )

    print(f"asr: {cfg.asr_url}   cleanup: {cfg.cleanup_url} {cfg.cleanup_model} "
          f"({'on' if cleanup_on else 'off'})   interval: {args.interval}s")
    print(f"speak for {args.seconds:.0f} s ...  ([clean] = polished sentence, ~x~ = tentative)")

    def on_audio(indata, frames, when, status) -> None:
        live.feed(indata.copy())

    stream = sd.InputStream(samplerate=cfg.sample_rate, channels=1, dtype="int16",
                            device=device, callback=on_audio)
    live.start()
    stream.start()
    try:
        time.sleep(args.seconds)
    except KeyboardInterrupt:
        pass
    finally:
        stream.stop()
        stream.close()

    released = time.time()
    text = live.finish()
    tail_ms = int((time.time() - released) * 1000)
    print()
    print(f"final ({tail_ms} ms after release, {live.passes} ASR passes):")
    print(text or "<empty - the engine would fall back to the whole-clip path>")
    print(f"raw:   {live.raw_text}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
