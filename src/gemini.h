#pragma once

#include <Arduino.h>

// ============================================================================
// gemini.h — HTTPS calls to Google's Gemini API
// ============================================================================
//
// Two API calls make up the whole "brain" of this device, both hitting the
// same generateContent REST endpoint but with different request/response
// shapes:
//   1. understandSpeech()  — send recorded audio,  get back a text reply
//   2. synthesizeSpeech()  — send that text reply,  get back spoken audio
//
// There's also textOnlyQuery(), a minimal helper used only for hardware
// bring-up testing (see WIFI_TEXT_TEST_MODE in config.h).
//
// All three make a single blocking HTTPS request each — there's no persistent
// connection, no session, no streaming; this stays simple on purpose (see the
// project's "non-goals" around not using the Live/streaming API).

// POST audio to Gemini for a spoken-language reply.
//   wavData / wavLen — the WAV-wrapped 16kHz/16-bit/mono recording (see
//                       audio.h's wavWrap())
//   replyTextOut      — filled with Gemini's text reply on success
// Returns true on success, false on any network/API failure (caller should
// treat this as recoverable — flash an error indicator and return to idle,
// not crash or hang).
bool understandSpeech(const uint8_t* wavData, size_t wavLen, String& replyTextOut);

// Simple text-only generateContent call, no audio involved. Used only by
// WIFI_TEXT_TEST_MODE (Build Phase 2 in the README) to sanity-check WiFi, TLS,
// and the API key in isolation, before the audio pipeline is ever exercised.
bool textOnlyQuery(const String& prompt, String& replyTextOut);

// POST text to Gemini's text-to-speech model.
//   text     — the reply text to have spoken aloud (typically Gemini's own
//              reply from understandSpeech())
//   pcmOut   — destination buffer for the decoded audio; must have room for
//              pcmCap bytes
//   pcmLenOut — on success, set to how many bytes of raw PCM were written to
//               pcmOut (fewer than pcmCap if the reply was short)
//   pcmCap    — capacity of pcmOut, in bytes
// Returns true on success, false if the request failed or the response was
// too large to fit in pcmCap.
bool synthesizeSpeech(const String& text, uint8_t* pcmOut, size_t& pcmLenOut, size_t pcmCap);
