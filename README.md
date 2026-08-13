# Fih — ESP32 Push-to-Talk Gemini Voice Assistant

A standalone voice assistant: press a button, talk, release, hear a spoken
reply. All "thinking" happens in the cloud via Google's Gemini API — there's
no local server, no relay machine, and no wake-word engine. Just the ESP32 and
an internet connection.

```
Press button → record → release button → send to Gemini →
receive text → send text to Gemini TTS → receive audio → play → idle
```

Two sequential HTTPS calls per interaction, no persistent connection, and no
multi-turn memory (each press is a fresh request) — this keeps the firmware
simple enough to build and debug as a solo hobby project.

---

## Table of contents

- [How it works](#how-it-works)
- [Hardware](#hardware)
- [Project layout](#project-layout)
- [Setup](#setup)
- [Build / upload / monitor](#build--upload--monitor)
- [Unit tests](#unit-tests)
- [Build phases (hardware bring-up)](#build-phases-hardware-bring-up)
- [How the Gemini calls work](#how-the-gemini-calls-work)
- [Memory budget](#memory-budget)
- [Things to spot-check once you have hardware](#things-to-spot-check-once-you-have-hardware)
- [Security / privacy notes](#security--privacy-notes)
- [Troubleshooting](#troubleshooting)
- [Testing checklist](#testing-checklist)
- [Future upgrades (not in v1 scope)](#future-upgrades-not-in-v1-scope)

---

## How it works

```
┌─────────────────────────────────────────────┐
│                  ESP32 board                 │
│                                               │
│  [Button] → [I2S Mic] → [PSRAM buffer]       │
│                              │                │
│                    (button released)          │
│                              ▼                │
│                     [WiFi + TLS/HTTPS]        │
└──────────────────────────────┬────────────────┘
                                │
                                ▼
                    Gemini API (generateContent)
                    — send base64 audio, get text back
                                │
                                ▼
                    Gemini TTS API (generateContent, audio out)
                    — send text, get base64 audio back
                                │
                                ▼
┌──────────────────────────────┬────────────────┐
│                              ▼                │
│                    [Decode + I2S Amp]         │
│                       [Speaker]               │
│                  ESP32 board (same device)    │
└─────────────────────────────────────────────┘
```

Firmware state machine (see `src/main.cpp`):

```
IDLE
  → (button press) → RECORDING (I2S capture into PSRAM buffer)
  → (button release) → UPLOADING (base64 encode + HTTPS POST #1 — audio understanding)
  → PROCESSING (await text response)
  → SYNTHESIZING (HTTPS POST #2 — text-to-speech)
  → PLAYING (decode + I2S playback)
  → IDLE
```

Because there's no persistent connection or background task, everything from
UPLOADING through PLAYING runs as one blocking sequence right after a
recording finishes (`processInteraction()` in `main.cpp`) — the state enum
mainly exists to drive the status LED, not to model concurrent work.

**Why this board, not ESP32-S3 or a plain WROOM:** the design holds two large
buffers at once (a ~470KB recording and up to ~300KB of decoded reply audio)
plus a WiFi/TLS stack — nowhere near enough room in the base ESP32's 520KB of
internal SRAM. A **WROVER module with PSRAM** is what makes the naive,
non-streaming approach in this firmware fit comfortably. See
[Memory budget](#memory-budget) below.

## Hardware

| Component | Notes |
|---|---|
| **ESP32 dev board — WROVER module (PSRAM)** | Hard requirement, not optional — see above. Look for "4MB PSRAM" or "8MB PSRAM" in the listing. |
| I2S MEMS microphone | e.g. INMP441 |
| I2S audio amp + speaker | e.g. MAX98357A breakout + small 4Ω speaker |
| Momentary pushbutton | Push-to-talk trigger |
| Status LED | Idle / recording / thinking / speaking / error indicator |

### Wiring

Default GPIO map, defined in [`include/pins.h`](include/pins.h) — remap there
to match your actual wiring; nothing else in `src/` hardcodes pin numbers.

| Signal | Pin | Peripheral |
|---|---|---|
| Mic BCLK (SCK) | GPIO26 | I2S0 RX |
| Mic WS (LRCLK) | GPIO25 | I2S0 RX |
| Mic DATA (SD) | GPIO33 | I2S0 RX |
| Amp BCLK | GPIO27 | I2S1 TX |
| Amp LRC (WS) | GPIO14 | I2S1 TX |
| Amp DIN | GPIO13 | I2S1 TX |
| Button | GPIO4 | active-low, internal pull-up |
| Status LED | GPIO2 | — |

Two independent I2S peripherals are used (I2S0 for the mic, I2S1 for the amp)
rather than time-sharing one bus — this design never records and plays back
at the same time (push-to-talk is strictly sequential), so there's no need for
full-duplex bus-sharing logic. Pins were chosen to avoid strapping pins (0, 2,
5, 12, 15 — GPIO2 is the one intentional exception, safe to use as an LED pin
post-boot) and the internal PSRAM/flash pins (6–11).

## Project layout

```
Fih/
├── platformio.ini             PlatformIO build config: the real firmware (wrover) +
│                                a host-machine unit test environment (native)
├── .gitignore                  Keeps include/config.h (secrets) out of version control
├── include/
│   ├── pins.h                   GPIO map + audio-format constants
│   └── config.h.example         Template for WiFi/API-key secrets — copy to config.h
├── src/
│   ├── main.cpp                  State machine: button, recording, LED, orchestration
│   ├── audio.h / audio.cpp        I2S mic capture + amp playback (hardware-dependent)
│   ├── gemini.h / gemini.cpp      Both HTTPS calls to Gemini, JSON, retry (hardware-dependent)
│   ├── base64.h / base64.cpp      Arduino String-facing base64 wrapper (hardware-dependent)
│   ├── base64_core.h / .cpp       The actual base64 algorithm — pure, unit-tested
│   └── wav.h / wav.cpp            WAV header construction — pure, unit-tested
└── test/
    └── test_native/test_main.cpp  Unity tests for base64_core + wav (see below)
```

`base64_core`/`wav` are split out from `base64`/`audio` specifically because they
have zero Arduino/ESP-IDF dependency — that's what makes it possible to unit-test
them on a plain desktop machine, with no ESP32 attached.

Every file has header comments at the top explaining its role, and inline
comments on the less-obvious logic (I2S configuration fields, the mic
bit-shift, WAV header layout, JSON field shapes, debounce logic, etc.) — read
top-to-bottom, each file should be understandable on its own.

## Setup

```bash
cp include/config.h.example include/config.h
```

Edit `include/config.h` with:
- your WiFi SSID/password (2.4GHz only — the ESP32 doesn't support 5GHz)
- a Gemini API key from https://aistudio.google.com/apikey

`config.h` is listed in `.gitignore` and will never be committed.

## Build / upload / monitor

Requires the [PlatformIO](https://platformio.org/) VSCode extension (or the
`pio` CLI, e.g. `pip3 install -U platformio`). Open this folder in VSCode and
use the PlatformIO sidebar, or from the terminal:

```bash
pio run              # build
pio run -t upload    # build + flash
pio device monitor    # serial monitor (115200 baud)
```

`platformio.ini` targets `board = upesy_wrover`, a generic WROVER dev board
profile. If your specific board doesn't behave well under that id (e.g. wrong
default flash size), switch to `board = esp32dev` — PSRAM support comes from
the `build_flags` (`-DBOARD_HAS_PSRAM`), not the board id, so either works.

This firmware has been build-verified with `pio run` (compiles cleanly against
`framework-arduinoespressif32 @ 3.20017`) but **not** hardware-tested — no
ESP32 was available in the environment this was built in. The steps below walk
through validating it on real hardware in stages.

## Unit tests

Most of this firmware — I2S audio, WiFi, the Gemini HTTPS calls — can only be
meaningfully tested on real hardware (that's what the build phases below are
for). But two pieces have no hardware dependency at all and get real,
automated unit tests that run on your own machine, no ESP32 required:

- **`base64_core`** — the base64 encode/decode algorithm
- **`wav`** — the 44-byte WAV header builder

Run them with:

```bash
pio test -e native
```

This covers known base64 test vectors, a full encode/decode round trip at
both small and realistic (~470KB) recording sizes, undersized-buffer error
handling, and every field in the WAV header at its exact byte offset. All 13
cases pass as of the last run.

`platformio.ini`'s `[env:native]` environment only compiles `base64_core.cpp`
and `wav.cpp` for this — `audio.cpp`, `gemini.cpp`, `base64.cpp`, and
`main.cpp` all depend on Arduino/ESP-IDF headers that don't exist on a
desktop target, so they're excluded from the test build (see the
`build_src_filter` comment there). Plain `pio run` / `pio run -t upload`
still only ever targets the real `wrover` environment — `default_envs =
wrover` keeps the native/test environment from being accidentally built or
flashed.

## Build phases (hardware bring-up)

Two compile-time toggles in `config.h` let you validate the hardware in
stages rather than debugging the whole pipeline at once:

1. **Audio loopback, no network.** Set `LOOPBACK_TEST_MODE 1`. Button
   press/release records to PSRAM and immediately plays it straight back
   through the speaker — no WiFi, no Gemini calls at all. Confirms mic, amp,
   and wiring before adding any cloud dependency.
2. **WiFi + basic HTTPS.** Set `LOOPBACK_TEST_MODE` back to `0` and
   `WIFI_TEXT_TEST_MODE 1`. On boot, the device connects to WiFi and fires one
   hardcoded text-only Gemini call, printing the reply to Serial. Confirms
   WiFi, TLS, and the API key work in isolation.
3. **Audio → text.** Both flags back to `0`. Press/release now runs the real
   `understandSpeech()` call — watch Serial for the transcribed reply text.
4. **Text → speech → playback.** Same build as above; a successful Phase 3 run
   continues straight into `synthesizeSpeech()` and plays the reply out loud.
   This is the full loop working end-to-end.
5. **Polish.** LED/state feedback, debounce tuning, max-recording cutoff, and
   graceful error handling are already implemented (see below) — this phase is
   about tuning constants (`MIC_GAIN_SHIFT`, `DEBOUNCE_MS`,
   `MAX_RECORDING_SECONDS`) to taste once phases 1–4 pass on your hardware.

## How the Gemini calls work

Both calls hit Google's `generateContent` REST endpoint
(`https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent`),
differing only in model and request/response shape. **Field names are
camelCase** (`inlineData`, `mimeType`), which is worth calling out since some
other Google API documentation defaults to snake_case.

**Call 1 — `understandSpeech()` (audio → text):**
- Model: `GEMINI_TEXT_MODEL` in `config.h` (a Flash/Flash-Lite model — Pro's
  free-tier daily cap is too restrictive for regular use)
- Request: one `text` part (a fixed instruction prompt) + one `inlineData`
  part containing the base64-encoded WAV recording, `mimeType: "audio/wav"`
- `generationConfig.maxOutputTokens` bounds the reply length, which in turn
  bounds the TTS audio length and downstream memory use
- Response: plain text at `candidates[0].content.parts[0].text`

**Call 2 — `synthesizeSpeech()` (text → speech):**
- Model: `GEMINI_TTS_MODEL` in `config.h`
- Request: the reply text, plus `generationConfig.responseModalities:
  ["AUDIO"]` and a `speechConfig.voiceConfig.prebuiltVoiceConfig.voiceName`
- Response: base64-encoded **raw PCM** audio at
  `candidates[0].content.parts[0].inlineData.data`, tagged with a mime type
  like `audio/L16;codec=pcm;rate=24000` — this is *not* a WAV file, just
  headerless 16-bit/24kHz/mono samples, decoded and fed straight to the I2S
  amp.

Both calls retry once (after a fixed delay) on an HTTP 429 (rate limited)
response, and return `false` on any other failure so `main.cpp` can flash an
error indicator and return to idle rather than crash or hang.

## Memory budget

Rough numbers for a 15-second max recording on a WROVER board (4MB or 8MB
PSRAM):

| Item | Size | Notes |
|---|---|---|
| Raw mic buffer (15s @ 16kHz/16-bit/mono) | ~470 KB | `wavBuf`/`pcmBuf` in `main.cpp`, held in PSRAM during recording |
| Base64-encoded upload body | ~625 KB | Built and freed inside `understandSpeech()` — 33% base64 inflation over the raw buffer |
| TLS session overhead | ~50–100 KB | mbedTLS record buffers + session state |
| Base64-encoded TTS response | ~100–300 KB | Bounded by `GEMINI_MAX_OUTPUT_TOKENS` — keep replies short |
| Decoded playback PCM buffer | 300 KB (allocated) | `ttsBuf` in `main.cpp`, same scale as the encoded response, un-inflated |
| **Rough peak total** | **~1.3–1.7 MB** | Fits comfortably on a 4MB WROVER board; more headroom on 8MB |

`main.cpp` logs `ESP.getFreePsram()` at boot and after buffer allocation —
compare against these numbers on your actual board rather than trusting the
estimate blindly.

## Things to spot-check once you have hardware

These are treated as configurable assumptions, not hardcoded facts, precisely
because they can drift as Google's API evolves:

- **Model names** (`GEMINI_TEXT_MODEL`, `GEMINI_TTS_MODEL` in `config.h`) —
  confirmed available as of Aug 2026 research, but verify against
  [Google's current model list](https://ai.google.dev/gemini-api/docs/models)
  before relying on them long-term, especially if a call starts failing with
  a "model not found" error.
- **TTS response `mimeType`** — `gemini.cpp` logs the actual `mimeType` string
  the TTS call returns. Check the serial log on your first successful TTS
  call to confirm it matches what `audioPlayFromBuffer()` assumes (raw
  16-bit/24kHz/mono PCM, no unwrapping).
- **Free-tier rate limits** — sources on exact RPM/RPD for the flash/flash-lite
  models conflicted during research. If you hit limits often in practice,
  widen `RETRY_DELAY_MS` in `gemini.cpp` or drop to a lighter model.
- **Mic gain** (`MIC_GAIN_SHIFT` in `audio.cpp`) — a starting-point bit-shift
  for converting the mic's 32-bit I2S frames to 16-bit PCM. If Phase 1
  loopback sounds too quiet, decrease it; if it clips, increase it.

## Security / privacy notes

- The Gemini API key lives on the device (in gitignored `config.h`). Acceptable
  for a personal device that stays on your own hardware — don't publish source
  or compiled firmware with a real key baked in.
- TLS uses `WiFiClientSecure::setInsecure()` (no certificate validation) for
  simplicity. Fine for a personal project; swap in a pinned Google root CA in
  `gemini.cpp`'s `postJson()` if you want stricter validation.
- Google's free tier allows using submitted prompts for model training/review
  (unlike the paid tier) — worth being a deliberate choice since this device
  records your voice.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Build fails with a PSRAM/heap error | Board doesn't actually have PSRAM, or `-DBOARD_HAS_PSRAM` build flag got removed from `platformio.ini` |
| Loopback recording is silent or very quiet | Check mic wiring against the pin table; try lowering `MIC_GAIN_SHIFT` in `audio.cpp` |
| Loopback recording clips/distorts | Raise `MIC_GAIN_SHIFT` in `audio.cpp` |
| `WIFI_TEXT_TEST_MODE` never connects | Wrong SSID/password in `config.h`, or a 5GHz-only network (unsupported) |
| Gemini calls fail with HTTP 4xx | Check `GEMINI_API_KEY` and that `GEMINI_TEXT_MODEL`/`GEMINI_TTS_MODEL` are still valid model names |
| Gemini calls fail with HTTP 429 repeatedly | Free-tier rate limit — widen `RETRY_DELAY_MS` in `gemini.cpp` or reduce request frequency |
| TTS playback sounds wrong (pitch/speed) | Confirm the logged `mimeType` from `synthesizeSpeech()` really is 24kHz — `SAMPLE_RATE_PLAYBACK` in `pins.h` must match |
| Device resets/crashes mid-recording | Likely a PSRAM allocation issue — check the free-PSRAM logs at boot against the [memory budget](#memory-budget) |

## Testing checklist

- [ ] Mic captures clean audio (check for clipping/noise via Phase 1 loopback)
- [ ] Speaker playback is clear at usable volume
- [ ] WiFi reconnects gracefully after a drop (handled in `loop()`)
- [ ] TLS handshake succeeds reliably under PSRAM + audio buffer memory pressure
- [ ] Button debounce doesn't cause false triggers or missed releases
- [ ] Max recording length cutoff works if button is held too long
- [ ] Behavior on API error/rate limit is graceful (rapid LED blink, no crash)

## Future upgrades (not in v1 scope)

Considered during planning, worth revisiting later, but add complexity not
needed for a first working build:

- **Relay server + Xiaozhi firmware** — fork Xiaozhi (ESP-IDF), run
  xiaozhi-esp32-server on a Pi/mini PC as a relay, configure Gemini as the LLM
  provider. Better for extending to more providers/tools later, but note
  Xiaozhi and most ESP-ADF voice projects target ESP32-S3 specifically — this
  would mean sourcing a different board.
- **Gemini Live API** — native audio-in/audio-out over a persistent WebSocket,
  for lower-latency, more natural conversation. Requires either a custom relay
  or significantly more firmware work to hand-roll the streaming protocol.
- **Wake-word mode** — swap push-to-talk for always-listening
  (WakeNet/MicroWakeWord) if hands-free becomes a priority.
- **Local/offline fallback** — self-hosted STT+SLM+TTS stack
  (whisper.cpp/Ollama/Piper) on a relay, for full privacy or no-internet
  operation.
