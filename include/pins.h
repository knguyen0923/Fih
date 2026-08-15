#pragma once

// ============================================================================
// pins.h — hardware pin map and audio format constants
// ============================================================================
//
// Every GPIO number and audio-format number the firmware uses lives in this one
// file. If you wire the board differently than the defaults below, this is the
// only file you should need to edit — src/audio.cpp, src/main.cpp, etc. all
// refer to these #defines rather than hardcoding pin numbers themselves.
//
// Default wiring assumes a generic ESP32 WROOM-32D dev board with:
//   - an INMP441 (or similar) I2S MEMS microphone
//   - two MAX98357A (or similar) I2S amplifiers, wired for stereo output
//   - a dual H-bridge DC motor driver + 2 DC motors, reacting to volume
//
// Pins were chosen to avoid:
//   - "strapping pins" (0, 2, 5, 12, 15) whose logic level at boot affects how
//     the chip starts up (e.g. which flash voltage it expects, whether it
//     boots into the flasher). Using them for normal I/O after boot is usually
//     fine, but it's safer for beginners to avoid them where possible.
//     (GPIO2 is the one exception here — see the LED note below.)
//   - GPIO 6-11, which are wired internally to the chip's own flash/PSRAM and
//     are not usable as general-purpose pins at all on WROVER modules.

// --- I2S0 (RX) — microphone input ---
// I2S is a 3-wire serial protocol for streaming audio samples:
//   BCLK (bit clock)   — toggles once per audio bit, keeps sender/receiver in sync
//   WS   (word select)  — toggles once per audio sample, marks left/right channel
//   DATA                — the actual audio sample bits, one wire per direction
#define PIN_MIC_BCLK 26
#define PIN_MIC_WS   25
#define PIN_MIC_DATA 33

// --- I2S1 (TX) — amplifier output ---
// A second, independent I2S peripheral drives the speaker. Using two separate
// peripherals (I2S0 for RX, I2S1 for TX) instead of time-sharing one bus is
// simpler because this design is strictly sequential — record, THEN play back,
// never both at once — so there's no need for full-duplex bus sharing logic.
//
// Two MAX98357A amp boards share these same three pins for true stereo
// output (one BCLK/WS/DATA bus carries both L and R channels, time-
// multiplexed — that's what I2S stereo means). Each board's channel-select
// pin (often labeled SD or GAIN, depending on the breakout revision) needs
// to be strapped per its own datasheet/silkscreen to pick left vs right —
// this varies enough between clones/revisions that it's called out here as
// a spot-check rather than a specific voltage/resistor value.
#define PIN_AMP_BCLK 27
#define PIN_AMP_WS   14
#define PIN_AMP_DATA 13

// --- Volume-reactive DC motors (dual H-bridge, e.g. Aideepen/L298N-style) ---
// Each motor gets 2 GPIOs (its H-bridge's two control inputs). Speed control
// with no reverse needed: PWM one pin with the volume-mapped duty cycle,
// hold the other LOW. This pattern works the same way across effectively
// every 2-pin-per-motor H-bridge chip (L298N, DRV8833, AT8870, TB6612...),
// which is why it's used here instead of assuming this specific board's
// exact pin semantics.
//
// The H-bridge's motor-supply rail must come from its own power source (the
// user's separate 5V supply) — NOT the ESP32's own 3.3V/5V rail, which isn't
// meant to source motor current. Tie all grounds (5V supply, H-bridge,
// ESP32) together.
#define PIN_MOTOR_A_IN1 16
#define PIN_MOTOR_A_IN2 17
#define PIN_MOTOR_B_IN1 18
#define PIN_MOTOR_B_IN2 19

// --- Button and status LED ---
// The button is wired active-low: one leg to this GPIO, the other to GND.
// INPUT_PULLUP (set in main.cpp) means the pin reads HIGH when not pressed and
// LOW when pressed, without needing an external resistor.
#define PIN_BUTTON 4
// GPIO2 is technically a strapping pin, but only during the boot moment itself;
// using it as a plain output afterward (as we do here) is standard practice —
// many ESP32 dev boards even have an onboard LED wired to this exact pin.
#define PIN_LED    2

// --- Audio format constants ---
// Capture (mic -> Gemini) and playback (Gemini TTS -> speaker) intentionally
// run at *different* sample rates. 16kHz is plenty for speech recognition and
// keeps the recording buffer small; 24kHz is simply the fixed rate Gemini's
// TTS model returns audio at, so playback must match it exactly or the speech
// will sound sped-up/slowed-down.
#define SAMPLE_RATE_CAPTURE   16000  // Hz, mic -> Gemini (16-bit, mono)
#define SAMPLE_RATE_PLAYBACK  24000  // Hz, Gemini TTS -> speaker (16-bit, mono upmixed to stereo) — fixed by the API, don't change
#define SAMPLE_RATE_BLUETOOTH 44100  // Hz, A2DP's fixed rate (16-bit, stereo) — not configurable, part of the Bluetooth audio spec

// Name advertised when a phone scans for this device to pair with.
#define BT_DEVICE_NAME "Fih Speaker"

// Hard cap on how long a single recording can be, enforced in main.cpp's
// recording loop. This exists so that a stuck or taped-down button can't grow
// the recording buffer indefinitely — see MAX_PCM_BYTES in main.cpp, which is
// derived from this value.
//
// This board (ESP32-WROOM-32D) has no PSRAM, so unlike a WROVER build, this
// buffer has to fit in the same ~320KB of internal SRAM as the WiFi/TLS
// stack. 5 seconds (~160KB, see main.cpp) is a conservative starting point,
// not a precisely derived constant — same spirit as MIC_GAIN_SHIFT in
// audio.cpp. Once real hardware is up, watch the ESP.getFreeHeap() logs in
// main.cpp: if a recording-heavy run still leaves plenty of headroom, this
// can go up; if understandSpeech()'s TLS handshake ever fails under memory
// pressure right after a long recording, bring it back down.
#define MAX_RECORDING_SECONDS 5
