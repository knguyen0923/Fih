#pragma once

#include <Arduino.h>

// ============================================================================
// base64.h — base64 encode/decode
// ============================================================================
//
// Gemini's REST API sends and receives raw binary audio as base64 text inside
// JSON (JSON has no native way to embed binary bytes). This is a small,
// self-contained base64 codec so the project doesn't need an external library
// dependency for something this simple. Standard alphabet
// (A-Z, a-z, 0-9, +, /), '=' padding — this is the same base64 variant used
// almost everywhere (email attachments, data: URLs, etc.).

// Encodes raw bytes into a base64 String. Every 3 input bytes become 4 output
// characters, which is why the encoded upload body is noticeably (~33%)
// larger than the raw recording — see the memory notes in main.cpp.
String base64Encode(const uint8_t* data, size_t len);

// Decodes a base64 String back into raw bytes, writing into `out`.
// `out` must have at least base64DecodedLen(in.length()) bytes of space.
// Returns the number of bytes actually written, or 0 if `out` was too small
// or `in` was empty/malformed.
size_t base64Decode(const String& in, uint8_t* out, size_t outCap);

// Upper bound on decoded size for a base64 string of the given length. Every 4
// base64 characters decode to at most 3 raw bytes, plus a little slack for
// safety — use this to size the buffer you pass to base64Decode().
inline size_t base64DecodedLen(size_t encodedLen) {
    return (encodedLen / 4) * 3 + 3;
}
