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
// Both understandSpeech() and synthesizeSpeech() stream their large payload
// (the uploaded WAV audio, and the decoded TTS reply, respectively) instead
// of holding it in memory whole — this board (ESP32-WROOM-32D) has no PSRAM,
// so there's no spare RAM to buffer either one in full. There's still just
// one blocking HTTPS request per call each — no persistent connection, no
// session (see the project's "non-goals" around not using the Live API).

// POST audio to Gemini for a spoken-language reply.
//   wavData / wavLen — the WAV-wrapped 16kHz/16-bit/mono recording (see
//                       audio.h's wavWrap()). The request body is built by
//                       base64-encoding wavData on the fly in small chunks as
//                       it's streamed to the socket, so no ~1.3x-larger
//                       base64 copy of it is ever held in memory.
//   replyTextOut      — filled with Gemini's text reply on success
// Returns true on success, false on any network/API failure (caller should
// treat this as recoverable — flash an error indicator and return to idle,
// not crash or hang).
bool understandSpeech(const uint8_t* wavData, size_t wavLen, String& replyTextOut);

// Simple text-only generateContent call, no audio involved. Used only by
// WIFI_TEXT_TEST_MODE (Build Phase 2 in the README) to sanity-check WiFi, TLS,
// and the API key in isolation, before the audio pipeline is ever exercised.
bool textOnlyQuery(const String& prompt, String& replyTextOut);

// POST text to Gemini's text-to-speech model, streaming the decoded reply
// audio out via callback as it arrives instead of returning it in one buffer.
//   text        — the reply text to have spoken aloud (typically Gemini's
//                 own reply from understandSpeech())
//   onPcmChunk  — called zero or more times, once per decoded chunk of raw
//                 16-bit/24kHz/mono PCM, in order, as the response streams
//                 in. The pointer passed to it is only valid for the
//                 duration of that call (it's a reused internal buffer).
// Returns true on success, false if the request or response failed. Note
// this can return false after already having delivered some audio via
// onPcmChunk — callers should treat a false return as "the reply may have
// been cut short," not "nothing was played."
bool synthesizeSpeech(const String& text, void (*onPcmChunk)(const uint8_t* pcm, size_t len));
