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
// called before every Gemini voice interaction (which needs WiFi) and
// bluetoothStart() resumes it afterward. The two are never active at once.

// Configures I2S1 for Bluetooth's fixed audio format (see
// audioSetPlaybackRate() in audio.cpp) and starts advertising/accepting an
// A2DP connection under BT_DEVICE_NAME (pins.h).
void bluetoothStart();

// Fully disconnects and stops the Bluetooth controller, freeing the radio
// for WiFi. Not a "pause" -- WiFi and Bluetooth Classic can't both be
// running reliably on this chip, so this needs to be a real teardown.
void bluetoothStop();
