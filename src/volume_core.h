#pragma once

#include <stddef.h>
#include <stdint.h>

// ============================================================================
// volume_core.h — pure loudness calculation, no Arduino/ESP-IDF dependency
// ============================================================================
//
// Drives the volume-reactive motors: turns a block of PCM samples into a
// single 0-255 loudness number via RMS (root-mean-square), the standard way
// to measure a signal's average power rather than being thrown off by a
// single loud/quiet sample. No dependency on Arduino or motor.h's PWM output,
// which is what makes this unit-testable on a plain desktop machine (see
// test/test_native/test_main.cpp) the same way base64_core/wav are.

// Computes the RMS loudness of `sampleCount` interleaved 16-bit PCM samples,
// scaled to 0-255. Works the same whether `samples` is mono or stereo --
// RMS is computed across every sample given to it regardless of channel
// layout, so the caller doesn't need to tell it which.
uint8_t computeVolumeLevel(const int16_t* samples, size_t sampleCount);
