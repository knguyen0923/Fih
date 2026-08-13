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
// Default wiring assumes a generic ESP32 WROVER dev board with:
//   - an INMP441 (or similar) I2S MEMS microphone
//   - a MAX98357A (or similar) I2S amplifier + speaker
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
#define PIN_AMP_BCLK 27
#define PIN_AMP_WS   14
#define PIN_AMP_DATA 13

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
#define SAMPLE_RATE_PLAYBACK  24000  // Hz, Gemini TTS -> speaker (16-bit, mono) — fixed by the API, don't change

// Hard cap on how long a single recording can be, enforced in main.cpp's
// recording loop. This exists so that a stuck or taped-down button can't grow
// the recording buffer indefinitely — see MAX_PCM_BYTES in main.cpp, which is
// derived from this value.
#define MAX_RECORDING_SECONDS 15
