# Fih — ESP32 Push-to-Talk Gemini Voice Assistant

A standalone voice assistant: press a button, talk, release, hear a spoken
reply. All "thinking" happens in the cloud via Google's Gemini API — there's
no local server, no relay machine, and no wake-word engine. Just the ESP32 and
an internet connection.

It's also a Bluetooth speaker the rest of the time: pair a phone and stream
music to it like any other Bluetooth speaker, with 2 DC motors that react to
whatever's currently playing (Bluetooth music or a Gemini TTS reply) by
speeding up/down with the volume.

```
Idle: paired phone streams music over Bluetooth → played on 2 speakers,
      motors react to volume
Press button → Bluetooth pauses, WiFi comes up → record → release button →
send to Gemini → receive text → send text to Gemini TTS → receive audio →
play (motors still reacting) → WiFi goes back down, Bluetooth resumes → idle
```

Two sequential HTTPS calls per Gemini interaction, no persistent connection,
and no multi-turn memory (each press is a fresh request) — this keeps that
part of the firmware simple enough to build and debug as a solo hobby
project. The Bluetooth/WiFi radio-switching needed to combine it with a
Bluetooth speaker is the one genuinely complex piece — see
[How Bluetooth and WiFi share one radio](#how-bluetooth-and-wifi-share-one-radio).

---

## Table of contents

- [How it works](#how-it-works)
- [How Bluetooth and WiFi share one radio](#how-bluetooth-and-wifi-share-one-radio)
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
┌───────────────────────────────────────────────────────────┐
│                       ESP32 board                          │
│                                                              │
│  Idle: [Phone] --Bluetooth A2DP--> [2x I2S Amp] --> [2 Speakers]
│                                          │                   │
│                                          ▼                   │
│                                  [2 DC Motors] (react to volume)
│                                                              │
│  [Button] → [I2S Mic] → [RAM buffer]                        │
│                              │                               │
│                    (button released)                        │
│                              ▼                               │
│              Bluetooth stops, [WiFi + TLS/HTTPS] comes up    │
└──────────────────────────────┬───────────────────────────────┘
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
┌──────────────────────────────┬───────────────────────────────┐
│                              ▼                                │
│                 [Decode + 2x I2S Amp] → [2 Speakers]           │
│                              │                                 │
│                              ▼                                 │
│                       [2 DC Motors] (react to volume)           │
│                                                                  │
│         WiFi goes back down, Bluetooth resumes — ESP32 board    │
│                          (same device throughout)                │
└──────────────────────────────────────────────────────────────────┘
```

Firmware state machine (see `src/main.cpp`):

```
IDLE (Bluetooth speaker: phone streams music, motors react to it)
  → (button press) → RECORDING (Bluetooth stops; WiFi connect kicked off in
     the background; I2S capture into RAM buffer)
  → (button release) → UPLOADING (streamed base64 encode + HTTPS POST #1 — audio understanding)
  → PROCESSING (await text response)
  → SYNTHESIZING (HTTPS POST #2 — text-to-speech, decoded and played back per
     chunk as it streams in; motors react to it too)
  → WiFi torn down, Bluetooth speaker resumes → IDLE
```

Because there's no persistent connection or background task on the Gemini
side, everything from UPLOADING through SYNTHESIZING runs as one blocking
sequence right after a recording finishes (`processInteraction()` in
`main.cpp`) — the state enum mainly exists to drive the status LED, not to
model concurrent work. There's no separate PLAYING step in this path any
more: `synthesizeSpeech()` plays each decoded chunk of the reply as it
arrives rather than waiting for the whole reply first (see
[Memory budget](#memory-budget)) — `PLAYING` as a state still exists only for
`LOOPBACK_TEST_MODE`'s direct mic-to-speaker playback, which is unrelated to
the Gemini calls. Bluetooth audio, by contrast, *is* handled by a background
task (owned by the `ESP32-A2DP` library, not this project's code) — see
below for why that doesn't conflict with the "no background tasks" design of
the Gemini path.

**Why this board:** an ESP32-WROOM-32D has no PSRAM, only ~320KB of internal
SRAM shared with the WiFi/TLS stack — nowhere near enough to hold a whole
recording *and* a whole decoded reply in memory at once, the way an earlier
WROVER-based design did. Streaming the base64-encoded upload and the decoded
TTS reply (neither is ever held in full — see
[How the Gemini calls work](#how-the-gemini-calls-work)) gets peak memory use
down to essentially one buffer: the raw recording. See
[Memory budget](#memory-budget) below.

## How Bluetooth and WiFi share one radio

The ESP32 has a single 2.4GHz radio, shared between WiFi and Bluetooth
Classic (the profile A2DP — Bluetooth audio streaming — needs). In practice,
the two don't reliably run at once: A2DP's constant audio streaming leaves
little room for WiFi, and vice versa ([confirmed via research](https://github.com/pschatzmann/ESP32-A2DP/wiki/WIFI-and-A2DP-Coexistence)).
Rather than fight that, this firmware switches between the two radio modes
instead of running them simultaneously:

- **Idle = Bluetooth speaker.** `bluetoothStart()` (`src/bluetooth.cpp`) is
  the default state coming out of `setup()`.
- **Button press → full radio switch.** `bluetoothStop()` disconnects and
  fully disables the Bluetooth controller (`a2dp_sink.end(true)` — confirmed
  by reading the `ESP32-A2DP` library's own source that the default,
  `end()`/`end(false)`, does *not* actually release the controller, which
  would defeat the point), then `WiFi.begin()` is kicked off immediately, in
  parallel with the recording that follows — so by the time the button is
  released, WiFi has often already finished connecting, hiding most of the
  switch latency (`loop()` in `main.cpp`).
- **After the Gemini interaction (or if the button tap was too brief to
  record anything), `exitVoiceMode()` tears WiFi back down and restarts
  Bluetooth** — proactively reconnecting to the phone it was just talking to
  (`connect_to()`, using an address this project saves itself, since
  `end()` always wipes the library's own "last connection" memory as part
  of its shutdown) rather than passively waiting for the phone to notice
  the speaker again.
- **Short tone cues** (`audioPlayTone()` in `audio.cpp`) mark both
  transitions — a full Bluetooth teardown isn't instant (the library's own
  shutdown sequence takes real time), so an otherwise-silent multi-second
  gap is made to read as an intentional "listening now" / "back to being a
  speaker" moment instead of the device seeming to have hung.

The real cost of this design is a noticeable delay (expect roughly 1-3
seconds, hardware-dependent) between releasing the button and the recording
actually starting to upload, on top of Gemini's own response time — this
hasn't been measured on real hardware yet (see
[Things to spot-check](#things-to-spot-check-once-you-have-hardware)). The
tone cues and proactive reconnect are meant to make that gap feel
deliberate rather than eliminate it — true audio "ducking" (music kept
playing, just quieter, while Gemini works) was considered and rejected: it
would require Bluetooth to keep actively streaming audio at the same time
WiFi is live, which is the one scenario the coexistence research above
found genuinely unreliable, not just slow.

## Hardware

| Component | Notes |
|---|---|
| **ESP32 dev board — WROOM-32D** | No PSRAM required — see above. `platformio.ini` targets `board = esp32dev`, a generic profile that fits this module. |
| I2S MEMS microphone | e.g. INMP441 |
| 2x I2S audio amp + speaker | 2x MAX98357A breakout + small 4Ω speakers — true stereo (see [Wiring](#wiring)), used for both Bluetooth music and Gemini TTS replies (mono, upmixed to stereo) |
| Dual H-bridge DC motor driver + 2 DC motors | e.g. an L298N-style dual-channel driver. Volume-reactive — see [How it works](#how-it-works) |
| Separate 5V (or similar) power supply for the motors | The H-bridge's motor-supply rail must **not** come from the ESP32's own 3.3V/5V rail |
| Momentary pushbutton | Push-to-talk trigger |
| Status LED | Idle / recording / thinking / speaking / error indicator |

Bluetooth pairing needs no additional hardware — it uses the ESP32's own
built-in radio (see [How Bluetooth and WiFi share one radio](#how-bluetooth-and-wifi-share-one-radio)).

### Wiring

Default GPIO map, defined in [`include/pins.h`](include/pins.h) — remap there
to match your actual wiring; nothing else in `src/` hardcodes pin numbers.

| Signal | Pin | Peripheral |
|---|---|---|
| Mic BCLK (SCK) | GPIO26 | I2S0 RX |
| Mic WS (LRCLK) | GPIO25 | I2S0 RX |
| Mic DATA (SD) | GPIO33 | I2S0 RX |
| Amp BCLK | GPIO27 | I2S1 TX — **both** amp boards |
| Amp LRC (WS) | GPIO14 | I2S1 TX — **both** amp boards |
| Amp DIN | GPIO13 | I2S1 TX — **both** amp boards |
| Button | GPIO4 | active-low, internal pull-up |
| Status LED | GPIO2 | — |
| Motor A speed (PWM) | GPIO16 | H-bridge IN1 |
| Motor A direction | GPIO17 | H-bridge IN2 — held LOW (forward-only, no reverse needed) |
| Motor B speed (PWM) | GPIO18 | H-bridge IN1 |
| Motor B direction | GPIO19 | H-bridge IN2 — held LOW |

Two independent I2S peripherals are used (I2S0 for the mic, I2S1 for the amp)
rather than time-sharing one bus — this design never records and plays back
at the same time (push-to-talk is strictly sequential), so there's no need for
full-duplex bus-sharing logic. Pins were chosen to avoid strapping pins (0, 2,
5, 12, 15 — GPIO2 is the one intentional exception, safe to use as an LED pin
post-boot) and the module's internal SPI flash pins (6–11).

**Both amp boards share the same 3 I2S1 pins** — that's how I2S stereo works
(one bus, L and R samples time-multiplexed on the same DATA line). Each
board's channel-select pin (labeled `SD` or `GAIN`, depending on the
breakout revision) needs to be strapped per its own datasheet/silkscreen to
pick left vs. right — this isn't guessed at here since it varies enough
between revisions to get wrong; check yours before wiring.

**Motors:** each motor uses 2 H-bridge control pins — PWM one (speed, volume-
mapped), hold the other LOW (this pattern works the same across effectively
every 2-pin-per-motor H-bridge chip, which is why the code doesn't assume a
specific one). The H-bridge's motor-supply rail needs its own power source
(see [Hardware](#hardware)) — tie all grounds (5V supply, H-bridge, ESP32)
together.

## Project layout

```
Fih/
├── platformio.ini             PlatformIO build config: the real firmware (esp32) +
│                                a host-machine unit test environment (native).
│                                Uses huge_app.csv partitioning -- the Bluetooth
│                                stack doesn't fit in the default OTA-sized partition.
├── .gitignore                  Keeps include/config.h (secrets) out of version control
├── include/
│   ├── pins.h                   GPIO map + audio-format constants
│   └── config.h.example         Template for WiFi/API-key secrets — copy to config.h
├── src/
│   ├── main.cpp                  State machine: button, recording, LED, Bluetooth/WiFi
│   │                               radio switching, orchestration
│   ├── audio.h / audio.cpp        I2S mic capture + stereo amp playback, including the
│   │                               Bluetooth/voice sample-rate switch (hardware-dependent)
│   ├── bluetooth.h / bluetooth.cpp  Bluetooth Classic A2DP sink ("Bluetooth speaker"
│   │                               mode) (hardware-dependent)
│   ├── motor.h / motor.cpp        Volume-reactive DC motor PWM output (hardware-dependent)
│   ├── gemini.h / gemini.cpp      Both HTTPS calls to Gemini — streaming upload body
│   │                               (Base64AudioBodyStream) and streaming TTS response
│   │                               decode, JSON, retry (hardware-dependent)
│   ├── base64_core.h / .cpp       The base64 algorithm, incl. a streaming decoder —
│   │                               pure, unit-tested
│   ├── volume_core.h / .cpp       RMS loudness calculation for the motors — pure,
│   │                               unit-tested
│   └── wav.h / wav.cpp            WAV header construction — pure, unit-tested
└── test/
    └── test_native/test_main.cpp  Unity tests for base64_core + volume_core + wav
```

`base64_core`/`volume_core`/`wav` have zero Arduino/ESP-IDF dependency — that's
what makes it possible to unit-test them on a plain desktop machine, with no
ESP32 attached.

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

`platformio.ini` targets `board = esp32dev`, a generic WROOM-32D dev board
profile, with `board_build.partitions = huge_app.csv` — the default
partition table splits the 4MB flash into two ~1.3MB OTA app slots, and the
Bluetooth Classic/A2DP stack doesn't fit in that (~1.7MB compiled, confirmed
by build). This project has no use for OTA updates anyway, so `huge_app.csv`
trades that away for a single ~3MB app partition instead.

This firmware has been build-verified with `pio run` (compiles cleanly against
`framework-arduinoespressif32 @ 3.20017`, currently RAM 18.5%/Flash 54.6% of
the huge_app partition, reported at the end of the build) but **not**
hardware-tested — no ESP32 was available in the environment this was built
in. The steps below walk through validating it on real hardware in stages.

## Unit tests

Most of this firmware — I2S audio, WiFi, Bluetooth, motor PWM, the Gemini
HTTPS calls — can only be meaningfully tested on real hardware (that's what
the build phases below are for). But three pieces have no hardware
dependency at all and get real, automated unit tests that run on your own
machine, no ESP32 required:

- **`base64_core`** — the base64 encode/decode algorithm
- **`volume_core`** — the RMS loudness calculation driving the motors
- **`wav`** — the 44-byte WAV header builder

Run them with:

```bash
pio test -e native
```

This covers known base64 test vectors, a full encode/decode round trip at both
small and realistic (~470KB) recording sizes, undersized-buffer error
handling, the streaming decoder against arbitrary chunk splits (1 byte at a
time, uneven 7-byte chunks, and a ~470KB buffer split into TLS-read-sized
~512-byte pieces — the same shapes `synthesizeSpeech()` actually feeds it in
`gemini.cpp`), the volume calculation against known cases (silence, full-scale,
half-scale, empty input), and every field in the WAV header at its exact byte
offset. All 20 cases pass as of the last run.

`platformio.ini`'s `[env:native]` environment only compiles `base64_core.cpp`,
`volume_core.cpp`, and `wav.cpp` for this — `audio.cpp`, `bluetooth.cpp`,
`motor.cpp`, `gemini.cpp`, and `main.cpp` all depend on Arduino/ESP-IDF
headers that don't exist on a desktop target, so they're excluded from the
test build (see the `build_src_filter` comment there). Plain `pio run` /
`pio run -t upload` still only ever targets the real `esp32` environment —
`default_envs = esp32` keeps the native/test environment from being
accidentally built or flashed.

## Build phases (hardware bring-up)

Two compile-time toggles in `config.h` let you validate the hardware in
stages rather than debugging the whole pipeline at once:

1. **Audio loopback, no network.** Set `LOOPBACK_TEST_MODE 1`. Button
   press/release records to RAM and immediately plays it straight back
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
6. **Bluetooth speaker + motors.** With phases 1-5 passing (and both test-mode
   flags back to `0`), pair a phone and confirm: music plays clearly through
   both speakers (check the channel-select wiring — see
   [Wiring](#wiring) — if it's mono, silent, or backwards), the motors react
   to its volume, a button press cleanly stops Bluetooth and switches to a
   normal voice interaction, and Bluetooth resumes afterward. This is the one
   phase most likely to need real debugging rather than constant-tuning —
   `Base64AudioBodyStream`/`streamTtsResponse()`/`BluetoothA2DPSink`'s data
   callback/the LEDC motor PWM are all new, hardware-untested code (see
   [Things to spot-check](#things-to-spot-check-once-you-have-hardware)).

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
  part containing the base64-encoded WAV recording, `mimeType: "audio/wav"` —
  the base64 text is generated and streamed straight to the socket a chunk at
  a time (`Base64AudioBodyStream` in `gemini.cpp`), so the ~1.3x-larger
  encoded copy of the recording never exists in memory whole
- `generationConfig.maxOutputTokens` bounds the reply length, which in turn
  bounds the TTS audio length
- Response: plain text at `candidates[0].content.parts[0].text` (small,
  bounded by `maxOutputTokens`, so read and parsed in one shot as before)

**Call 2 — `synthesizeSpeech()` (text → speech):**
- Model: `GEMINI_TTS_MODEL` in `config.h`
- Request: the reply text, plus `generationConfig.responseModalities:
  ["AUDIO"]` and a `speechConfig.voiceConfig.prebuiltVoiceConfig.voiceName`
- Response: base64-encoded **raw PCM** audio at
  `candidates[0].content.parts[0].inlineData.data`, tagged with a mime type
  like `audio/L16;codec=pcm;rate=24000` — this is *not* a WAV file, just
  headerless 16-bit/24kHz/mono samples. The response body is read
  incrementally (not buffered whole) and each decoded chunk is handed to a
  callback that plays it immediately, so a full reply's worth of decoded PCM
  never needs to fit in memory at once either — see `streamTtsResponse()` in
  `gemini.cpp`. Since this board's amp is wired for true stereo (2 boards —
  see [Wiring](#wiring)), each mono chunk gets upmixed (duplicated to both
  L and R) before playback — see `audioPlayMonoAsStereo()` in `audio.cpp`.

Both calls retry once (after a fixed delay) on an HTTP 429 (rate limited)
response, and return `false` on any other failure so `main.cpp` can flash an
error indicator and return to idle rather than crash or hang.

## Memory budget

This board (ESP32-WROOM-32D) has no PSRAM — only ~320KB of internal SRAM,
shared with the Arduino core, WiFi, and TLS. An earlier revision of this
firmware targeted a WROVER module and held three large buffers in PSRAM at
once (a ~470KB recording, a ~625KB base64 upload copy, and a 300KB decoded
TTS buffer); none of that fits here. Instead, the upload and the TTS reply
are both streamed (see [How the Gemini calls work](#how-the-gemini-calls-work))
so only one large buffer remains:

| Item | Size | Notes |
|---|---|---|
| Raw mic buffer (`MAX_RECORDING_SECONDS` @ 16kHz/16-bit/mono) | ~160 KB at the default 5s cap | `wavBuf`/`pcmBuf` in `main.cpp` — the only large buffer left, plain internal heap (`malloc()`), held for the device's whole uptime |
| Base64-encoded upload body | none held at once | Streamed a ~1KB-encoded-chunk at a time straight to the TLS socket (`Base64AudioBodyStream` in `gemini.cpp`) |
| TLS session overhead (transient, per call) | ~40–50 KB | mbedTLS record buffers + session state, active only for the duration of each `understandSpeech()`/`synthesizeSpeech()` call — but see below, this now only runs while Bluetooth is fully stopped |
| Decoded TTS reply audio | none held at once | Decoded and played a chunk at a time as the response streams in (`streamTtsResponse()` in `gemini.cpp`) |
| Bluetooth Classic (A2DP) stack (transient, while active) | reportedly tens of KB of heap ([GitHub issue](https://github.com/pschatzmann/ESP32-A2DP/issues/64)) | Only active while Bluetooth is running — i.e. never at the same time as the TLS overhead above, by design (see [How Bluetooth and WiFi share one radio](#how-bluetooth-and-wifi-share-one-radio)) |
| Bluetooth Classic (A2DP) stack — **flash**, not RAM | ~750 KB compiled in | Confirmed by build: this pushed total flash use from ~950KB to ~1.7MB, past the default ~1.3MB OTA partition — this project now uses `board_build.partitions = huge_app.csv` (~3MB app partition, no OTA) instead |

`MAX_RECORDING_SECONDS` (in `pins.h`) is deliberately conservative — 5 seconds
by default — because `wavBuf` is allocated once at boot and held permanently,
while TLS still needs its own ~40-50KB on top of that during every request.
`main.cpp` logs `ESP.getFreeHeap()` at boot and after buffer allocation;
compare those numbers against actual behavior on your board (does a long
recording followed immediately by a Gemini call still succeed?) before
raising `MAX_RECORDING_SECONDS` — this can only be tuned on real hardware,
not estimated from a datasheet. The Bluetooth stack being fully stopped
before every such call (rather than merely idle) is what keeps its heap use
out of this same budget — if that teardown were ever relaxed to a "pause"
instead, this budget would need re-checking against both costs at once.

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
- **`MAX_RECORDING_SECONDS`** (`pins.h`) — 5 seconds is a conservative
  starting point given this board's lack of PSRAM, not a measured value (no
  ESP32 was available to measure actual free heap in the environment this was
  built in). Watch the `ESP.getFreeHeap()` logs in `main.cpp` and adjust — see
  [Memory budget](#memory-budget).
- **Streaming upload/playback themselves** — `Base64AudioBodyStream` and
  `streamTtsResponse()` in `gemini.cpp` are new, hardware-untested code
  (build-verified only). Watch for truncated/garbled replies or upload
  failures on your first few real interactions, which would point to a bug in
  the streaming logic rather than a network fluke.
- **The `ESP32-A2DP` and LEDC (motor PWM) API calls in `bluetooth.cpp`/
  `motor.cpp`** — written from memory of these libraries and confirmed to
  *compile* against the actual installed versions, but never run — real
  pairing/audio-quality/PWM behavior is unverified.
- **Amp channel-select wiring** — whichever pin (`SD`/`GAIN`) picks left vs.
  right on each MAX98357A board; get this backwards and stereo audio plays
  in the wrong channel or a channel plays silent. See [Wiring](#wiring).
- **Bluetooth/WiFi radio-switch timing** — how long the switch actually takes
  in practice, and whether starting `WiFi.begin()` at the start of recording
  (rather than after button release) meaningfully hides that latency, hasn't
  been measured on real hardware.
- **Proactive Bluetooth reconnect** — `bluetooth.cpp`'s `connect_to()` call
  (using a peer address this project saves itself before tearing Bluetooth
  down) is confirmed to compile against the real library, but whether it
  actually reconnects faster/more reliably than passively waiting hasn't
  been tested. If it doesn't help in practice, `set_auto_reconnect()` (also
  present in the library) is a fallback worth trying instead.
- **Tone cue volume/pitch** (`TONE_ENTER_VOICE_HZ`/`TONE_EXIT_VOICE_HZ`/
  `TONE_DURATION_MS` in `main.cpp`, amplitude in `audioPlayTone()` in
  `audio.cpp`) — picked to be audible-but-gentle without hearing it on real
  speakers; adjust to taste.
- **Motors holding their last position** — if Bluetooth playback pauses
  without a track change, no new PCM arrives to update `motorUpdateFromPcm()`,
  so the motors will hold whatever speed they were last at rather than
  returning to 0. No separate "playback stopped" hook is wired up for v1 —
  a deliberate simplification, not a bug.

## Security / privacy notes

- The Gemini API key lives on the device (in gitignored `config.h`). Acceptable
  for a personal device that stays on your own hardware — don't publish source
  or compiled firmware with a real key baked in.
- TLS uses `WiFiClientSecure::setInsecure()` (no certificate validation) for
  simplicity. Fine for a personal project; swap in a pinned Google root CA in
  `gemini.cpp` (each of `postJson()`, `postStreamed()`, and
  `synthesizeSpeech()` opens its own `WiFiClientSecure`) if you want stricter
  validation.
- Google's free tier allows using submitted prompts for model training/review
  (unlike the paid tier) — worth being a deliberate choice since this device
  records your voice.
- Bluetooth pairing uses the `ESP32-A2DP` library's default settings, which
  (unverified — see [spot-check list](#things-to-spot-check-once-you-have-hardware))
  likely means no PIN/passkey ("Just Works" pairing) — any nearby phone could
  connect and play audio through it. Fine for a personal speaker in a home;
  harden this (a fixed PIN, or not advertising when not wanted) if that's a
  concern in your setting.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Device halts at boot printing "Failed to allocate recording buffer" | `wavBuf` didn't fit in free heap — lower `MAX_RECORDING_SECONDS` in `pins.h` |
| Loopback recording is silent or very quiet | Check mic wiring against the pin table; try lowering `MIC_GAIN_SHIFT` in `audio.cpp` |
| Loopback recording clips/distorts | Raise `MIC_GAIN_SHIFT` in `audio.cpp` |
| `WIFI_TEXT_TEST_MODE` never connects | Wrong SSID/password in `config.h`, or a 5GHz-only network (unsupported) |
| Gemini calls fail with HTTP 4xx | Check `GEMINI_API_KEY` and that `GEMINI_TEXT_MODEL`/`GEMINI_TTS_MODEL` are still valid model names |
| Gemini calls fail with HTTP 429 repeatedly | Free-tier rate limit — widen `RETRY_DELAY_MS` in `gemini.cpp` or reduce request frequency |
| TTS playback sounds wrong (pitch/speed) | Confirm the logged `mimeType` from `synthesizeSpeech()` really is 24kHz — `SAMPLE_RATE_PLAYBACK` in `pins.h` must match |
| TTS playback cuts off partway through | `streamTtsResponse()` in `gemini.cpp` couldn't find the closing quote on the `data` field before the connection closed — check WiFi stability, or that `GEMINI_MAX_OUTPUT_TOKENS` hasn't been raised to where the reply is unexpectedly long |
| Understanding/TTS calls fail right after a long recording | TLS's own ~40-50KB transient allocation didn't fit alongside `wavBuf` — lower `MAX_RECORDING_SECONDS` in `pins.h` (see [Memory budget](#memory-budget)) |
| Device resets/crashes mid-recording | Check the free-heap logs at boot against the [memory budget](#memory-budget) |
| Build fails with "program size ... greater than maximum allowed" | The `board_build.partitions = huge_app.csv` line got removed/changed in `platformio.ini` — the Bluetooth stack doesn't fit in the default OTA-sized partition (see [Build / upload / monitor](#build--upload--monitor)) |
| Bluetooth music is mono, silent on one speaker, or channels are swapped | Check each MAX98357A board's channel-select (`SD`/`GAIN`) pin wiring against its own datasheet — see [Wiring](#wiring) |
| Motors don't respond, or respond backwards from what you'd expect | Check `PIN_MOTOR_A/B_IN1/IN2` wiring against the pin table, and that the H-bridge's motor-supply rail is actually powered (separately from the ESP32) |
| Voice interaction feels sluggish to start after a button press | Expected, by design — see [How Bluetooth and WiFi share one radio](#how-bluetooth-and-wifi-share-one-radio) for the tradeoff being made and why |
| Phone can't find/pair with the device | Confirm `bluetoothStart()` actually ran (i.e. `LOOPBACK_TEST_MODE`/`WIFI_TEXT_TEST_MODE` are both `0`) and that a voice interaction isn't currently in progress (Bluetooth is intentionally off during one) |

## Testing checklist

- [ ] Mic captures clean audio (check for clipping/noise via Phase 1 loopback)
- [ ] Speaker playback is clear at usable volume
- [ ] WiFi reconnects gracefully after a drop (handled in `loop()`)
- [ ] TLS handshake succeeds reliably under `wavBuf` + WiFi/TLS memory pressure
- [ ] Button debounce doesn't cause false triggers or missed releases
- [ ] Max recording length cutoff works if button is held too long
- [ ] Behavior on API error/rate limit is graceful (rapid LED blink, no crash)
- [ ] Streamed upload/playback don't glitch under real network jitter
      (`Base64AudioBodyStream`/`streamTtsResponse()` in `gemini.cpp` are
      hardware-untested — build-verified only)
- [ ] Bluetooth pairing works reliably and both speakers play in true stereo
      with correct L/R channels
- [ ] Motors visibly react to both Bluetooth music and Gemini TTS reply volume
- [ ] A button press cleanly stops Bluetooth, completes a voice interaction,
      and Bluetooth resumes afterward — repeatedly, not just once
- [ ] The WiFi-connect-during-recording overlap actually reduces perceived
      latency vs. connecting only after button release
- [ ] After a voice interaction, the phone reconnects to Bluetooth promptly
      (proactive `connect_to()` working as intended, not just falling back to
      the phone noticing on its own)
- [ ] The enter/exit tone cues are actually audible and pleasant at real
      speaker volume, not jarring or too quiet to notice

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
