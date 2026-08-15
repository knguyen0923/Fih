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

// Plays raw 16-bit stereo PCM straight out the amp, at whatever sample rate
// I2S1 is currently configured for (see audioSetPlaybackRate()) -- this is
// the format both Bluetooth audio (already stereo) and audioPlayMonoAsStereo()
// (below) hand it.
void audioPlayFromBuffer(const uint8_t* pcm, size_t len);

// Changes I2S1's sample rate without a full driver reinstall (channel format
// stays fixed at stereo -- see configureAmpI2S() in audio.cpp). Used to
// switch the amp between SAMPLE_RATE_BLUETOOTH (idle, A2DP) and
// SAMPLE_RATE_PLAYBACK (during a Gemini TTS reply).
void audioSetPlaybackRate(uint32_t sampleRate);

// Plays raw 16-bit MONO PCM (e.g. Gemini's TTS output) through the same
// stereo-configured I2S1 bus that audioPlayFromBuffer() writes to, by
// duplicating each sample into an L+R pair first. Needed because this board
// wires 2 amps for true stereo, so I2S1 is always in stereo format -- mono
// audio has to be upmixed rather than sent as-is.
void audioPlayMonoAsStereo(const uint8_t* monoPcm, size_t len);

// Plays a short sine-wave tone through the amp, blocking until it finishes.
// `sampleRate` must match whatever I2S1 is currently configured for (see
// audioSetPlaybackRate()) -- used as a brief UI cue around the Bluetooth/
// WiFi radio switch (see main.cpp), so an otherwise-silent multi-second gap
// while Bluetooth fully tears down/reconnects reads as an intentional "the
// assistant is listening now" (or "back to being a speaker") moment,
// instead of the device seeming to have hung or broken.
void audioPlayTone(uint32_t sampleRate, uint32_t freqHz, uint32_t durationMs);
