# Fih — Wiring and Test Guide

Fih is an ESP32-WROOM-32D push-to-talk voice assistant that doubles as a Bluetooth speaker with two volume-reactive DC motors. This guide covers the pin map, the automated tests, and the checks that still need a person.

## Pin map

Defined in `include/pins.h`. Nothing else in `src/` hardcodes a pin number.

| Signal | GPIO | Notes |
|---|---|---|
| Mic BCLK | 19 | I2S0 RX |
| Mic WS | 21 | I2S0 RX |
| Mic DATA | 22 | I2S0 RX |
| Amp BCLK | 4 | I2S1 TX, shared by both amp boards |
| Amp WS (LRC) | 18 | I2S1 TX, shared by both amp boards |
| Amp DIN | 16 | I2S1 TX, shared by both amp boards |
| Button | 17 | Active-low, internal pull-up |
| Status LED | 2 | |
| Motor A IN1 | 25 | PWM speed |
| Motor A IN2 | 26 | Held low |
| Motor B IN1 | 27 | PWM speed |
| Motor B IN2 | 32 | Held low |

The motors need their own 5V supply for the H-bridge. Do not power them from the ESP32's rail.

## Setup

1. Install [PlatformIO](https://platformio.org/).
2. Copy `include/config.h.example` to `include/config.h` and fill in your WiFi SSID, WiFi password, and a Gemini API key (https://aistudio.google.com/apikey). `config.h` is gitignored.
3. The platform is pinned to `espressif32@6.9.0` (Arduino core 2.0.17) in `platformio.ini`. Unpinned, a fresh install pulls Arduino core 3.x, which breaks the build (`ledcSetup` removed, WiFi headers moved). If you see errors like `'WiFiClient' was not declared`, delete the `.pio` folder and rebuild.

## Automated tests

### 1. Native tests (no board needed)

```
pio test -e native
```

These 35 tests cover the pure logic, the parts with no Arduino or ESP-IDF dependency:

| Module | What it checks |
|---|---|
| `base64_core` | Known vectors, round trips at recording size, the streaming decoder against arbitrary chunk splits, undersized-buffer handling |
| `wav` | Every WAV header field at its byte offset, buffer-too-small handling |
| `volume_core` | RMS loudness for silence, full-scale, half-scale and empty input |
| `json_scan_core` | Finds the `data` and `mimeType` fields in Gemini's TTS reply: compact and pretty-printed JSON, whitespace around the colon, a missing key, input cut off mid-whitespace, and non-string values |
| `pcm_align_core` | Streamed PCM chunks always come out as whole 16-bit samples: odd bytes are carried to the next chunk, and an arbitrary split reproduces the original stream exactly |
| `pins.h` | The pin checks below |

- `test_no_gpio_assigned_twice` fails if two signals share a GPIO.
- `test_pins_are_valid_esp32_gpios` fails if a pin is a flash pin (6–11) or out of range.

These check `pins.h` only. They cannot tell you whether your physical wires match the file.

### 2. On-device tests (board attached and wired)

```
pio test -e esp32_test
```

The suite has 22 tests in `test/test_device/test_main.cpp`. It uses real WiFi and real Gemini API quota. Tap near the mic and listen for the beep while it runs.

| Area | What it checks | What a failure suggests |
|---|---|---|
| Memory | The recording buffer fits in the heap | Lower `MAX_RECORDING_SECONDS` |
| Button | Reads HIGH when untouched | Shorted, wired active-high, or the pin is shared |
| LED | Pin toggles | LED pin wiring |
| Motors | PWM duty is 0 at init and on silence, rises with loudness, and returns to 0 on stop. IN2 pins stay low | Motor pin or logic problem |
| Mic | Returns samples within 2 s, and the samples vary | Mic wiring or pins |
| Speaker | The tone takes about 300 ms (within 150 ms) at its own sample rate, and mono-to-stereo playback returns | Amp wiring, pins, or sample-rate handling |
| WAV | A full-size recording wraps correctly | Header builder bug |
| Bluetooth | Stop frees the controller, restart works, 3 stop/start cycles don't leak more than 8 KB of heap, mute/unmute don't crash | Radio-switching bug |
| WiFi and Gemini | WiFi connects, text query, TTS stream (checks for truncation and odd-sized chunks), audio upload, WiFi teardown then Bluetooth resumes | Credentials, network, or API issue |

The motor test reads the PWM register. It does not prove the motor spins.

## What needs a person

Automated tests cannot cover these. Check them by hand.

- [ ] **Motors spin.** Play music or a TTS reply and watch both motors react to volume. Confirm the H-bridge supply is powered.
- [ ] **Button press.** The suite only checks the idle state. Press the button and confirm the LED and serial output react.
- [ ] **Left/right speaker order.** Play stereo audio and confirm each speaker plays its own channel. If they are swapped, mono, or silent, fix the amp channel-select wiring.
- [ ] **Bluetooth pairing.** Pair a phone named "Fih Speaker" and confirm stereo music plays.
- [ ] **Music speed.** The speaker follows the sample rate the phone negotiates (44.1 or 48 kHz). If a phone's music sounds slow and low-pitched, that rate handling isn't working.
- [ ] **Audio quality.** Listen for pitch or speed artifacts. Tune `MIC_GAIN_SHIFT` in `src/audio.cpp` if the audio is too quiet or clipping.
- [ ] **Full flow.** Press the button, speak, release, and hear a Gemini reply. Confirm Bluetooth resumes afterward. Repeat several times.
- [ ] **Clean reply audio.** Listen for static or garbling in the spoken reply. That would point at the streaming/alignment code.
- [ ] **Held button.** Hold the button past the recording cap. The device should answer once and then wait for you to release, not start another recording.
- [ ] **Recording cap.** Check the serial log at boot. If it prints "Recording cap reduced to ...", the full 5 seconds didn't fit in one block of heap and the device is using a shorter limit.

## Notes

- The on-device suite compiles cleanly but has not yet been run on real hardware, so expect to adjust a few tests the first time.
- The native tests cover the parsing and alignment logic, but nothing exercises the real Gemini reply end to end except the on-device `test_gemini_tts_streams_pcm`, so run that early.
- Open risk: whether `bluetoothStop()` leaves Bluetooth restartable on real hardware is unverified. The on-device Bluetooth restart test is the check, so run it first.
- WiFi and Bluetooth Classic share one radio on the ESP32. The firmware fully stops Bluetooth for each Gemini round-trip and restarts it afterward.
- See `README.md` for the full architecture, API shapes, and troubleshooting table.
