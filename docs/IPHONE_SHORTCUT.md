# Flow on iPhone (Shortcuts + Tailscale)

No app to build — a Shortcut records audio, posts it to the whisper server on
your whisper server over the tailnet, and puts the transcript on the clipboard / share sheet.

Prereq: Tailscale app installed and connected on the iPhone (same tailnet).

## Create the Shortcut

1. Shortcuts app → + → name it **Flow**.
2. Add action **Record Audio** (Media) — "Finish Recording": *On Tap*.
3. Add action **Get Contents of URL**:
   - URL: `http://YOUR-WHISPER-SERVER:8890/inference`
   - Method: **POST**
   - Request Body: **Form**
   - Add field → type **File**, key `file`, value = *Recorded Audio* variable
   - Add field → type **Text**, key `response_format`, value `json`
   - Add field → type **Text**, key `language`, value `en`
4. Add action **Get Dictionary Value** — key `text` from *Contents of URL*.
5. Add three **Replace Text** actions (all with **Regular Expression ON**,
   each taking the previous *Updated Text* as input — the first takes the
   dictionary value). Whisper emits segment newlines mid-sentence and
   sometimes dialogue "- " markers:
   1. Find `(?m)^\s*-\s*` → replace with nothing (dash markers)
   2. Find `\s+` → replace with a single space (collapse newlines)
   3. Find `^\s+|\s+$` → replace with nothing (trim edges)
6. Add action **Copy to Clipboard** (input: the final *Updated Text*).
6. Optional: add **Show Result** to see the transcript, or **Share** to feed
   the share sheet directly.

## Use it

- Add to Home Screen, or say "Hey Siri, Flow", or trigger from the Action
  Button / Back Tap (Settings → Accessibility → Touch → Back Tap).
- Speak, tap stop, transcript is on the clipboard — paste anywhere.

Server does the m4a→wav conversion itself (verified: AAC/m4a accepted,
~0.2s for an 11s clip). Works from anywhere as long as Tailscale is up.

## Optional cleaned-text variant

Duplicate the Shortcut, and after step 5 add a second **Get Contents of URL**:
- URL: `http://YOUR-OLLAMA-SERVER:11434/api/chat`, POST, Request Body: **JSON**
- Body (use Text action then "Get contents"):
  model `hf.co/Qwen/Qwen3-14B-GGUF:Q5_K_M`, messages = system cleanup prompt +
  user text, `stream` false, `think` false
- Then Get Dictionary Value `message.content` → Copy to Clipboard.
