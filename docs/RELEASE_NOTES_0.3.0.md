The one where the words show up while you're still talking.

**Live transcription.** Hold a key and a small dark bar appears at the bottom of the screen. Words land in it every second and a half — solid once whisper has said the same thing twice in a row, dimmed while they're still tentative. whisper isn't a streaming model, so this is the same trick the commercial tools use: re-transcribe the audio since the last settled point and keep what two consecutive passes agree on. On release only the last couple of seconds are left to transcribe, so long dictations paste almost at once instead of after a wait proportional to how long you talked. The bar grows upward as text arrives (to about 40% of the screen, then scrolls), wraps properly, breaks long URLs, and never takes focus away from where you're typing.

**Cleanup while you talk.** With the cleaned key (Key 2 / F9) each sentence goes to the LLM the moment it's complete and swaps into the bar with a short fade when the tidied version comes back — the "appears, then corrects itself" effect. The model gets the previous thirty finished words as context so corrections that span sentences read right, a warm-up request fires the moment you press the key so the first sentence isn't waiting on a cold model, runaway replies are capped, and release now waits at most a few seconds for cleanup before pasting the raw words instead of up to twenty-five. The raw key (Key 1 / F8) shows the same live text and never touches the LLM.

**After each paste.** The old dropdown is a text box: `\s` for a space (the default), `\n` for a new line, `\t` for a tab, anything else literally, empty for nothing. So the next dictation lands with a gap and you don't have to.

**Smaller things**
- Both live pieces have switches under Feedback & clipboard: *Transcribe while you talk* and *Live transcript overlay*. Turn live off and you get the 0.2 behaviour.
- The paste keystroke goes through a direct Win32 `SendInput` now rather than the `keyboard` library, and fethr keeps a rolling log next to its settings file (`%APPDATA%\fethr\fethr.log`) with the window each paste was aimed at — pythonw has no console, so before this a failure left no trace.
- New tools in `hardware/tools/`: `live_probe.py` (watch the live passes against your own servers), `cleanup_bench.py` (time the cleanup path), `paste_probe.py` (try five ways of pasting into whatever you click on — useful when an app ignores fethr's Ctrl+V), `elevation_check.ps1`.

No firmware change; the 0.2.1 sidecar image is current.
