#pragma once

#include <Arduino.h>

// ============================================================================
// bluetooth.h — Bluetooth Classic A2DP sink ("Bluetooth speaker" mode)
// ============================================================================
//
// Wraps the pschatzmann/ESP32-A2DP library's BluetoothA2DPSink so a phone can
// pair with this device and stream music to it, played through the same
// amp/speaker hardware Gemini TTS replies use (see audio.cpp).
//
// This is the device's default idle behavior (see main.cpp) -- the ESP32 has
// one radio shared between WiFi and Bluetooth Classic, so bluetoothStop() is
// called once recording finishes for a voice interaction (which needs WiFi)
// and bluetoothStart() resumes it afterward. The two are never active at
// once. bluetoothMute()/bluetoothUnmute() handle the window between
// button-press and bluetoothStop() actually running -- see their comments.

// Configures I2S1 for Bluetooth's fixed audio format (see
// audioSetPlaybackRate() in audio.cpp) and starts advertising/accepting an
// A2DP connection under BT_DEVICE_NAME (pins.h).
void bluetoothStart();

// Fully disconnects and stops the Bluetooth controller, freeing the radio
// for WiFi. Not a "pause" -- WiFi and Bluetooth Classic can't both be
// running reliably on this chip, so this needs to be a real teardown. This
// is NOT instant (on the order of seconds) -- see bluetoothMute() for the
// cheap, immediate alternative used right at button-press.
void bluetoothStop();

// Immediately silences Bluetooth audio output (and stops feeding the
// motors) without touching the underlying connection -- just a flag flip,
// unlike bluetoothStop()'s multi-second teardown. Used right at
// button-press, before mic capture starts, so: (1) capture can begin with
// zero delay (bluetoothStop() being slow would otherwise delay it and lose
// the start of what's said), and (2) background Bluetooth music doesn't
// bleed into the recording while the user is talking. bluetoothStop() is
// still called afterward, once recording has finished, to actually free
// the radio for WiFi.
void bluetoothMute();

// Reverses bluetoothMute() without a full bluetoothStart() -- used when a
// button tap turns out to be too brief to record anything, so Bluetooth
// (never actually stopped, just muted) doesn't need a full restart.
void bluetoothUnmute();
