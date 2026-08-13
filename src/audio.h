#pragma once

#include <Arduino.h>
#include "wav.h" // re-exports WAV_HEADER_SIZE and wavWrap() for existing callers (e.g. main.cpp)

// ============================================================================
// audio.h — I2S microphone capture and speaker playback
// ============================================================================
//
// This module owns both audio directions:
//   - I2S0 (RX) reads from the microphone into a caller-provided PSRAM buffer
//   - I2S1 (TX) writes to the amplifier from a caller-provided PCM buffer
//
// The two directions never run at the same time in this design (push-to-talk
// is strictly record-THEN-play), so they're modeled as two independent,
// always-configured peripherals rather than one bus that gets reconfigured
// back and forth between recording and playback.
//
// WAV header construction (WAV_HEADER_SIZE, wavWrap()) lives in wav.h instead
// of here, since it's pure logic with no I2S/hardware dependency — see that
// file's comment for why that separation matters (native unit testing).

// Configures I2S0 (RX, mic) and I2S1 (TX, amp) as two independent peripherals —
// this design never records and plays back at the same time, so there's no need
// to share/reconfigure a single bus between directions.
void audioInit();

// Reads one chunk of 16-bit/16kHz/mono PCM samples from the mic into buf.
// chunkBytes must be even (whole 16-bit samples). Returns bytes actually written
// to buf (may be less than chunkBytes on an I2S read error/timeout).
size_t audioReadChunk(uint8_t* buf, size_t chunkBytes);

// Plays raw 16-bit/24kHz/mono PCM straight out the amp — this is exactly the
// format Gemini TTS returns, no unwrapping needed beyond base64 decode.
void audioPlayFromBuffer(const uint8_t* pcm, size_t len);
