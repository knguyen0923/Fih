#pragma once

#include <Arduino.h>

// ============================================================================
// motor.h — volume-reactive DC motor output (dual H-bridge)
// ============================================================================
//
// Drives 2 DC motors (see PIN_MOTOR_A/B_IN1/IN2 in pins.h) so their speed
// follows the loudness of whatever audio is currently playing -- Bluetooth
// music (bluetooth.cpp) or a Gemini TTS reply (main.cpp's playTtsChunk()).
// Both motors always mirror the same loudness level for v1 -- simplest
// starting point, easy to split later if you want them to react differently.
//
// Speed control uses one PWM'd pin + one pin held LOW per motor (no reverse
// needed for a volume-reactive effect) -- see the wiring comment in pins.h.

// Configures PWM on both motors' pins and holds them at 0 speed. Call once
// from setup().
void motorInit();

// Computes the loudness of one PCM chunk (via volume_core.h) and updates
// both motors' PWM duty to match. `pcm` is interleaved 16-bit samples --
// works the same whether it's mono (TTS) or stereo (Bluetooth music).
void motorUpdateFromPcm(const uint8_t* pcm, size_t len);

// Zeroes both motors' PWM duty. Called at boot and whenever entering voice
// mode (no audio plays through the speaker during the Gemini upload/process
// steps, so nothing should be driving the motors then).
void motorStop();
