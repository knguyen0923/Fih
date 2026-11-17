# Fih — Wiring and Test Guide

ESP32-WROOM-32D push-to-talk Gemini voice assistant and Bluetooth speaker with two volume-reactive motors. This guide covers the pin map, the automated tests, and the manual checks.

## Pin map

Defined in `include/pins.h`.

| Signal | GPIO | Notes |
|---|---|---|
| Mic BCLK / WS / DATA | 19 / 21 / 22 | I2S0 RX |
| Amp BCLK / WS (LRC) / DIN | 4 / 18 / 16 | I2S1 TX, shared by both amp boards |
| Button | 17 | Active-low, internal pull-up |
| Status LED | 2 | |
| Motor A IN1 / IN2 | 25 / 26 | PWM speed / held low |
| Motor B IN1 / IN2 | 27 / 32 | PWM speed / held low |

Power the H-bridge motor supply separately, not from the ESP32's rail.

## Setup

1. Install [PlatformIO](https://platformio.org/).
2. Copy `include/config.h.example` to `include/config.h` and fill in your WiFi SSID, WiFi password and a [Gemini API key](https://aistudio.google.com/apikey). The file is gitignored.
3. `platformio.ini` pins `espressif32@6.9.0` (Arduino core 2.0.17). Unpinned, installs pull core 3.x, which breaks the build (`'WiFiClient' was not declared`, `ledcSetup` removed). If you see those errors, delete `.pio` and rebuild.

## Native tests (no board)

```
pio test -e native
```

35 tests of the pure logic:

| Module | Checks |
|---|---|
| `base64_core` | Known vectors, round trips, streaming decode across arbitrary splits, undersized buffers |
| `wav` | Every header field at its byte offset, undersized buffers |
| `volume_core` | RMS loudness for silence, full-scale, half-scale, empty input |
| `json_scan_core` | Finds `data` and `mimeType` in Gemini's TTS reply: compact and pretty-printed JSON, whitespace around the colon, missing key, truncated input, non-string values |
| `pcm_align_core` | Streamed PCM always comes out as whole 16-bit samples; any split reproduces the original stream |
| `pins.h` | No GPIO assigned twice; no flash pins (6–11) or out-of-range pins |

The pin tests check the file, not your physical wiring.

## On-device tests (board wired and attached)

22 tests in `test/test_device/test_main.cpp`. They use real WiFi and Gemini API quota. Tap near the mic and listen for the beep while they run.

1. Wire the board per the pin map and fill in `config.h`.
2. Plug the ESP32 in over USB.
3. Run:
   ```
   pio test -e esp32_test
   ```
   Or, in VS Code: **Project Tasks → esp32_test → Advanced → Test**. It takes a couple of minutes; don't unplug the board.
4. If it picks the wrong port, add `--upload-port <port>` (`COM3` on Windows, `/dev/cu.usbserial-XXXX` on a Mac). If the upload won't start, hold the board's BOOT button while it connects.

Each test prints `[PASSED]` or `[FAILED]`, with a message saying what to check. To run only some tests, comment out the other `RUN_TEST(...)` lines in `setup()`; `-f` can't filter within one file. Run the Bluetooth restart test first.

| Area | Checks | A failure suggests |
|---|---|---|
| Memory | Recording buffer fits in the heap | Lower `MAX_RECORDING_SECONDS` |
| Button | Reads HIGH untouched | Shorted, wired active-high, or shared pin |
| LED | Pin toggles | LED wiring |
| Motors | Duty is 0 at init and on silence, rises with loudness, returns to 0 on stop; IN2 stays low | Motor pin or logic |
| Mic | Samples arrive within 2 s and vary | Mic wiring or pins |
| Speaker | Tone lasts ~300 ms (±150) at its own sample rate; mono-to-stereo playback returns | Amp wiring, pins, sample rate |
| WAV | Full-size recording wraps correctly | Header builder |
| Bluetooth | Stop frees the controller, restart works, 3 cycles leak under 8 KB, mute/unmute don't crash | Radio switching |
| WiFi and Gemini | WiFi connects; text query; TTS stream (no truncation or odd-sized chunks); audio upload; WiFi teardown then Bluetooth resumes | Credentials, network, API |

The motor test reads the PWM register; it doesn't prove the motor spins.

## Manual checks

- [ ] **Motors:** play music or a TTS reply; both motors react to volume. Check the H-bridge supply.
- [ ] **Button:** the suite only checks idle. Press it; the LED and serial output react.
- [ ] **Speaker channels:** each speaker plays its own channel. Swapped, mono or silent means fix the amp channel-select wiring.
- [ ] **Bluetooth:** pair "Fih Speaker"; stereo music plays at normal speed (slow and low-pitched means the negotiated 44.1/48 kHz rate isn't followed).
- [ ] **Audio quality:** no pitch or speed artifacts; tune `MIC_GAIN_SHIFT` in `src/audio.cpp` if too quiet or clipping.
- [ ] **Full flow:** press, speak, release, hear a clean Gemini reply (static or garbling points at the streaming code), and Bluetooth resumes. Repeat several times.
- [ ] **Held button:** hold past the recording cap; the device answers once, then waits for release.
- [ ] **Recording cap:** if the boot log prints "Recording cap reduced to ...", the full 5 seconds didn't fit in one heap block.

## Notes

- The on-device suite compiles but has never run on hardware; expect to adjust a few tests.
- `test_gemini_tts_streams_pcm` is the only end-to-end check of the real Gemini reply; run it early.
- Open risk: whether `bluetoothStop()` leaves Bluetooth restartable on real hardware is unverified. The Bluetooth restart test is the check.
- See `README.md` for architecture, API shapes and troubleshooting.
