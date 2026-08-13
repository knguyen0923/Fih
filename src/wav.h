#pragma once

#include <stddef.h>
#include <stdint.h>

// ============================================================================
// wav.h — minimal WAV header construction, no Arduino/ESP-IDF dependency
// ============================================================================
//
// A WAV file is just a fixed-size header followed by raw PCM samples. This
// logic has nothing to do with I2S hardware, so it lives in its own
// dependency-free file (unlike audio.h/.cpp, which does depend on ESP-IDF's
// I2S driver) — that's what makes it possible to unit-test natively (see
// test/test_native/test_main.cpp) without any ESP32 hardware attached.

// Size, in bytes, of the WAV header wavWrap() prepends.
constexpr size_t WAV_HEADER_SIZE = 44;

// Prepends a 44-byte WAV header (describing 16kHz/16-bit/mono PCM — the
// capture format used throughout this project) to raw PCM audio data, so
// Gemini's API can identify the format from the file itself.
//   pcm     — raw recorded samples (no header)
//   pcmLen  — length of `pcm` in bytes
//   out     — destination buffer for header + pcm combined
//   outCap  — total capacity of `out`, must be >= WAV_HEADER_SIZE + pcmLen
// Returns the total number of bytes written (WAV_HEADER_SIZE + pcmLen), or 0
// if `outCap` was too small to fit everything.
size_t wavWrap(const uint8_t* pcm, size_t pcmLen, uint8_t* out, size_t outCap);
