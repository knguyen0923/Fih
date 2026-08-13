#pragma once

#include <Arduino.h>

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

// Size, in bytes, of the minimal WAV header wavWrap() prepends. A WAV file is
// just this fixed 44-byte header followed by raw PCM sample data.
constexpr size_t WAV_HEADER_SIZE = 44;

// Configures both I2S peripherals. Call this once from setup(), after the
// pins in pins.h are finalized for your wiring.
void audioInit();

// Reads one chunk of 16-bit/16kHz/mono PCM samples from the microphone into
// `buf`. `chunkBytes` must be even (a whole number of 16-bit samples).
// Blocks until that many bytes are captured (or an I2S error occurs).
// Returns the number of bytes actually written to `buf` — normally equal to
// chunkBytes, but may be less if the I2S read failed partway through.
size_t audioReadChunk(uint8_t* buf, size_t chunkBytes);

// Prepends a 44-byte WAV header (describing 16kHz/16-bit/mono PCM) to raw PCM
// audio data, so Gemini's API can identify the format from the file itself.
//   pcm     — raw recorded samples (no header)
//   pcmLen  — length of `pcm` in bytes
//   out     — destination buffer for header + pcm combined
//   outCap  — total capacity of `out`, must be >= WAV_HEADER_SIZE + pcmLen
// Returns the total number of bytes written (WAV_HEADER_SIZE + pcmLen), or 0
// if `outCap` was too small to fit everything.
size_t wavWrap(const uint8_t* pcm, size_t pcmLen, uint8_t* out, size_t outCap);

// Plays raw 16-bit/24kHz/mono PCM straight out the amplifier. This is exactly
// the format Gemini's TTS response comes back in (after base64 decoding) —
// there's no WAV header to strip, since it was never wrapped in one to begin
// with; it's a headerless stream of samples.
void audioPlayFromBuffer(const uint8_t* pcm, size_t len);
