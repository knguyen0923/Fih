#pragma once

#include <Arduino.h>
#include "base64_core.h" // pure algorithm -- also re-exports base64DecodedLen()

// ============================================================================
// base64.h — Arduino-facing base64 codec
// ============================================================================
//
// Gemini's REST API sends and receives raw binary audio as base64 text inside
// JSON (JSON has no native way to embed binary bytes). This header exposes
// the convenient Arduino String-based API the rest of the firmware uses;
// the actual encode/decode algorithm lives in base64_core.h/.cpp, which has
// no Arduino dependency and is what's covered by the unit tests in
// test/test_native (see that folder for how to run them).

// Encodes raw bytes into a base64 String. Every 3 input bytes become 4 output
// characters, which is why the encoded upload body is noticeably (~33%)
// larger than the raw recording — see the memory notes in main.cpp.
String base64Encode(const uint8_t* data, size_t len);

// Decodes a base64 String back into raw bytes, writing into `out`.
// `out` must have at least base64DecodedLen(in.length()) bytes of space.
// Returns the number of bytes actually written, or 0 if `out` was too small
// or `in` was empty/malformed.
size_t base64Decode(const String& in, uint8_t* out, size_t outCap);
