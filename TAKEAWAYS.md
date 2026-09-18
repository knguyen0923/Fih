# Building Fih — Project Takeaways

A retrospective on building Fih: an ESP32-based push-to-talk voice
assistant that's also a Bluetooth speaker with volume-reactive DC motors —
one device, one radio, switching cleanly between two jobs. Built solo,
2026-08-13 to 2026-09-16 (9 commits over 34 days), from empty repo through
a full board swap (PSRAM → no-PSRAM mid-project) and six code review
passes, to a clean build with no hardware bring-up yet.

## What it does

Press a button, talk, release, hear a Gemini-generated spoken reply — no
wake word, no relay server, no persistent connection, just two sequential
HTTPS calls to Gemini per interaction. The rest of the time it's an A2DP
Bluetooth speaker: a phone streams music to two speakers, and two DC
motors react to whatever's currently playing (Bluetooth music or a TTS
reply) by speed.

The hard constraint underneath both halves: an ESP32 has one radio, and
WiFi and Bluetooth Classic don't reliably coexist on it. Every button
press has to fully tear down Bluetooth, bring up WiFi for the Gemini
round-trip, then reverse that — cleanly, every single time, for the life
of the device, not just once.

## Tech stack

| Layer | Choice | Why |
|---|---|---|
| MCU | ESP32-WROOM-32D (no PSRAM) | The board actually on hand — differs from the WROVER (PSRAM) module the firmware was originally designed against, forcing a rework (see below) |
| Framework | Arduino (via PlatformIO, `board = esp32dev`) | Familiar API surface over ESP-IDF, with PlatformIO's native-environment support making desktop unit testing possible |
| Audio I/O | Dual I2S (mic on I2S0, amp playback on I2S1) | Simultaneous capture and playback without one bus contending with the other |
| Bluetooth | `ESP32-A2DP` (pschatzmann) | A2DP sink with a raw PCM callback, wired straight into the existing I2S playback path and the motor driver |
| Cloud AI | Gemini (speech-to-text + text-to-speech, two sequential HTTPS calls) | No relay server or persistent connection needed — matches the "press, talk, release" interaction shape exactly |
| Testing | Unity (PlatformIO's `native` environment), TDD-adjacent | Runs on desktop with zero hardware dependency, by construction — only possible because the codec/WAV/volume logic was deliberately split out of anything Arduino- or ESP-IDF-specific |

## Architecture

```
Idle: phone --(Bluetooth A2DP)--> ESP32 --(I2S1)--> 2 speakers
                                     |
                                     +--> motor.cpp (volume-reactive PWM)

Button press:
  bluetoothMute()  (instant flag flip — safe mid-recording)
  motorStop()
        |
        v
  I2S0 mic capture --> WAV buffer (SRAM, no PSRAM)
        |
  button release
        v
  bluetoothStop()  (real multi-second radio teardown — only now, never earlier)
  WiFi connect
        |
        v
  understandSpeech()  --streams base64 upload-->  Gemini STT
        |
  synthesizeSpeech()  <--streams decoded PCM--  Gemini TTS
        |
        v
  I2S1 playback (streamed, chunk by chunk) --> 2 speakers
        |
  exitVoiceMode(): motorStop() -> exit tone -> bluetoothStart() -> WiFi off
```

Two pure-logic modules (`base64_core`, `volume_core`, `wav`) have zero
Arduino/ESP-IDF dependency — that's what makes them the only things
actually exercised by the `native` test environment.

## What I actually learned, by area

### Designing around one shared radio

The single most consequential design decision was accepting "WiFi and
Bluetooth Classic can't both be live" as a hard constraint rather than
trying to work around it (true ducking — keeping music playing quietly
during a Gemini call — was discussed and explicitly rejected once
research confirmed simultaneous use is unreliable, not just slow). Once
that was accepted, the rest of the design fell out of it: mute (instant,
safe during recording) had to be a different operation from stop (a real
multi-second teardown, safe only after recording finishes) — conflating
the two was the root cause of the most severe bug the second review pass
found (see below).

### A board swap mid-project changes what "correct" means, not just what's slow

The firmware was originally designed against a PSRAM-equipped WROVER
module; the actual hardware turned out to be a PSRAM-less WROOM-32D.
That's not a performance tweak, it's a memory-budget rewrite: both Gemini
HTTPS payloads (the mic upload and the TTS reply) had to move from
buffered-whole to streamed-through, so only the raw recording buffer
needs to fit in the ~320KB of internal SRAM this board actually has. Base64
encoding/decoding, chunked-transfer parsing, and PCM playback all had to
become streaming operations instead of single-shot ones as a direct
consequence.

### Iterative review passes catch different bugs than a single pass does

Six separate `/code-review --level high` passes were run across this
project, each one specifically told what the prior passes had already
found so it wouldn't re-report them — and each one still found something
real, including the most severe bug of the whole project on pass six,
after five prior passes had already looked at the same file. The pattern
that kept recurring: a fix for one bug (e.g., `end(true)` correctly
freeing the radio for WiFi) gets reasoned about narrowly ("does this
achieve X") rather than broadly ("is this safe to call more than once in
the device's lifetime") — and it takes a differently-scoped pass, not a
sharper one, to catch that gap. The fifth and sixth passes were
deliberately diff-scoped vs. whole-tree-scoped respectively, which is
exactly why the sixth caught something the first five, all diff-scoped
against a moving `HEAD`, never landed on.

### A landmine you don't trigger is still a bug worth fixing

The third review pass found a state-corruption path in the base64
streaming decoder that wasn't actually reachable by any current call
site — the buffers involved happen to always be sized large enough. It
got fixed anyway (fail closed on overflow instead of corrupting decoder
state for the rest of the stream), because "not currently exploitable"
and "safe" are different claims, and the second one is what a future
caller — or a future buffer-size change — actually needs to be true.

### Reading the vendored library source beats trusting its docs or its name

Two of the most severe bugs found (`bluetoothStop()`'s `end(true)`
permanently disabling restart; `readWithDeadline()`'s TLS-error path
silently defeating its own timeout) were only caught by reading the
actual vendored source of a third-party library or the ESP32 core itself
— `ESP32-A2DP`'s `end(true)` path and `WiFiClientSecure::available()`'s
negative-on-error behavior, respectively — rather than trusting what the
function name or its documentation implied. A plausible-sounding library
call is still worth reading before relying on it for something that runs
once per interaction for the life of the device.

### Working with an AI coding assistant deliberately

- **Re-scope the review, don't just re-run it.** The same reviewer
  re-running the same diff-scoped review converges on nothing new past a
  couple of passes; switching from diff-scoped to whole-tree-scoped (pass
  six) or splitting into parallel domain-specific sub-reviews (audio/
  codec, Gemini HTTP, Bluetooth/motor state machine, cleanup) surfaced
  real bugs the narrower framing had missed.
- **A "not real" finding still needs the same rigor as a real one.** One
  flagged concern (`bluetoothStop()` dereferencing a possibly-null
  address) was checked against the library's actual source and confirmed
  never null — worth recording as verified-not-real rather than silently
  dropped, so a future pass doesn't have to re-investigate the same
  question from scratch.
- **Draw a hard line between reversible and not.** Local edits, tests,
  and builds happened freely; nothing here ever touched a production or
  live environment. No real hardware exists to flash yet, which made this
  boundary easy to hold — but the same rule (read anywhere, write only to
  non-production) is the standing instruction this project was built
  under regardless.

## By the numbers

- **9 commits**, empty repo to a clean, fully-reviewed build, over 34 days
  (2026-08-13 to 2026-09-16) — including one full board-swap rework
  (WROVER/PSRAM design → WROOM-32D/no-PSRAM reality) mid-project
- **+4,212 / −877 lines** across the project's git history
- **1,681 lines of firmware source** (`src/*.cpp`) plus a 457-line, 24-case
  native Unity test suite — runnable on desktop with zero ESP32 hardware,
  by construction (the only code it exercises has no Arduino/ESP-IDF
  dependency)
- **Test suite: 20 cases at initial hardware-targeted build → 24 cases**
  after the third review pass added regression coverage for the
  streaming-decoder state-corruption fix, a malformed-base64-tail case,
  `INT16_MIN` in the volume calc, and a corrected realistic max-recording-
  size roundtrip test — all 24 passing throughout every subsequent pass
- **6 code review passes**, each scoped to avoid re-reporting the prior
  passes' findings: found and fixed **1 most-severe + 3 high + 2 moderate
  + several minor/doc-only bugs**, plus one deliberately-left-partially-
  open race (documented, not silently dropped) and one duplication cleanup
  extracting 4 shared helper functions
- **1 permanent-outage-class bug** (pass six): `bluetoothStop()`'s
  `end(true)` call permanently disabled Bluetooth restart after the
  *first* voice interaction of the device's entire lifetime — invisible
  to five prior review passes because each reasoned about whether the
  call achieved its immediate goal, not whether it was safe to call more
  than once
- **Build footprint: RAM 18.5% / 327,680 bytes, Flash 54.6% → 54.7% /
  3,145,728 bytes**, stable across every review pass from the first
  hardware-targeted build onward (measured statically, before the
  runtime recording buffer's own allocation)
- **20s → tightened to a genuinely-enforceable 20s**: the TTS stream
  wall-clock timeout was added in pass three, then found in pass four to
  only be checked *between* read calls (not within one, so a single call
  could still block far longer) — fixed by rewriting the read primitive
  to only ever consume already-available bytes, making the deadline
  actually per-call enforceable rather than best-effort
- **1 doc-accuracy correction with a ~6x error**: a comment claiming the
  TLS stream's per-character read timeout was "30s by default" was
  corrected to ~5s once pass five traced it to `HTTPClient`'s own
  override of `Stream::setTimeout()`'s default — the kind of gap that
  would have led a future reader to tune `TTS_STREAM_TIMEOUT_MS` against
  a number six times too large

## What's next

Nothing in this codebase has run on real hardware yet — every finding
above came from static review and a 24-case desktop test suite, not from
an actual button press. The next real milestone is Build Phase 1 (audio
loopback on real mic/amp hardware), which is also where the one
genuinely-unresolved-in-review question gets answered: whether the I2S
mic driver's `ONLY_LEFT` channel mode clocks one sample per cycle (the
code's current assumption) or two with one stale — flagged by pass six as
needing an actual listen-for-pitch-artifacts check, not another read of
conflicting SDK header comments.
