# Fih — project status for Claude / future-you

This file tracks what's been done and what's left, so picking this project
back up later (in Claude Code or otherwise) doesn't require re-deriving
context. See [README.md](README.md) for the full technical writeup — this
file is just status + a checklist.

## What this project is

ESP32 (WROVER, PSRAM) push-to-talk voice assistant. Press a button, talk,
release, hear a Gemini-generated spoken reply. No wake word, no relay server,
no persistent connection — two sequential HTTPS calls to Gemini per
interaction. Full architecture/rationale is in the README.

## What's done

- [x] Project scaffolded as a PlatformIO project (`board = upesy_wrover`,
      Arduino framework)
- [x] Full firmware written for all 5 build phases:
  - `src/audio.cpp` — dual I2S (mic capture on I2S0, amp playback on I2S1),
    WAV header builder
  - `src/gemini.cpp` — both Gemini HTTPS calls (`understandSpeech`,
    `synthesizeSpeech`) plus a `textOnlyQuery` test helper, with retry-on-429
  - `src/base64.cpp` — self-contained base64 codec
  - `src/main.cpp` — state machine, button debounce, LED status, WiFi
    reconnect, `LOOPBACK_TEST_MODE` / `WIFI_TEXT_TEST_MODE` bring-up toggles
- [x] Every file has explanatory header + inline comments (I2S config fields,
      WAV layout, JSON shapes, debounce logic, etc.) — this was an explicit
      ask, not just terse comments
- [x] `pio run` build-verified — compiles cleanly (no ESP32 hardware was
      available in the environment this was built in, so **nothing has been
      tested on real hardware yet**)
- [x] Comprehensive README written (architecture, wiring table, setup, build
      phases, API shapes, memory budget, troubleshooting table)
- [x] Pushed to GitHub: https://github.com/knguyen0923/Fih (private)

## What's still left (in order)

1. **Wire the hardware** — mic (INMP441-style), amp+speaker (MAX98357A-style),
   button, LED — per the pin table in the README. Update `include/pins.h` if
   your wiring differs from the defaults.
2. **Fill in real secrets** — `include/config.h` currently has placeholder
   WiFi/API-key values (copied from `config.h.example`). Edit it with your
   actual WiFi SSID/password and a real Gemini API key from
   https://aistudio.google.com/apikey.
3. **Build Phase 1 — audio loopback** — set `LOOPBACK_TEST_MODE 1` in
   `config.h`, flash, confirm recording/playback sounds right. Tune
   `MIC_GAIN_SHIFT` in `audio.cpp` if too quiet/clipping.
4. **Build Phase 2 — WiFi sanity** — `LOOPBACK_TEST_MODE 0`,
   `WIFI_TEXT_TEST_MODE 1`, confirm a Gemini text reply prints over Serial.
5. **Build Phase 3 — audio → text** — both flags `0`, confirm
   `understandSpeech()` transcribes/replies correctly over Serial.
6. **Build Phase 4 — full loop** — same build as Phase 3; confirm TTS
   playback works end to end.
7. **Build Phase 5 — polish** — tune `DEBOUNCE_MS`, `MAX_RECORDING_SECONDS`,
   error-handling behavior to taste once the above all pass.
8. **Spot-check the assumptions called out in the README** — these were
   researched but not hardware-verified:
   - `GEMINI_TEXT_MODEL` / `GEMINI_TTS_MODEL` names are still valid
   - TTS response `mimeType` actually matches the assumed
     16-bit/24kHz/mono raw PCM (logged to Serial by `gemini.cpp`)
   - Free-tier rate limits in practice (retry delay may need tuning)
9. **(Optional/future)** — anything from the README's "Future upgrades"
   section: wake-word mode, Gemini Live API streaming, local/offline fallback.

## Known simplifications (not bugs — deliberate v1 tradeoffs)

- LED doesn't animate *during* a blocking Gemini HTTPS call, only snaps to the
  new pattern when the call starts (loop() isn't running mid-call). Revisit
  with a non-blocking HTTP state machine if this bothers you in practice.
- TLS uses `setInsecure()` (no certificate pinning) — acceptable for a
  personal device, documented in the README's security section.
- No multi-turn conversation memory — every button press is a fresh,
  independent request.
