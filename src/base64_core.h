#pragma once

#include <stddef.h>
#include <stdint.h>

// ============================================================================
// base64_core.h — pure base64 algorithm, no Arduino/ESP-IDF dependency
// ============================================================================
//
// This is the actual codec logic, operating purely on raw byte buffers with
// no dependency on Arduino's String class or any ESP32-specific headers.
// That's deliberate: it means this file compiles and runs on a plain desktop
// machine, which is what makes it possible to unit-test (see
// test/test_native/test_main.cpp) without any ESP32 hardware attached.
//
// base64.h/.cpp wraps these functions with the Arduino String-based API the
// rest of the firmware actually calls.

// Exact encoded length (including '=' padding) for `len` input bytes. Every
// 3 input bytes become exactly 4 output characters.
size_t base64EncodedLen(size_t len);

// Upper bound on decoded size for a base64 string of the given length. Every
// 4 base64 characters decode to at most 3 raw bytes (plus a little slack).
size_t base64DecodedLen(size_t encodedLen);

// Encodes `len` bytes from `data` into `out` as base64 text (not
// null-terminated). `out` must have capacity for at least
// base64EncodedLen(len) bytes. Returns the number of characters written, or
// 0 if `outCap` was too small.
size_t base64EncodeCore(const uint8_t* data, size_t len, char* out, size_t outCap);

// Decodes `inLen` base64 characters from `in` into `out`. `out` must have
// capacity for at least base64DecodedLen(inLen) bytes. Returns the number of
// bytes written, or 0 if `out` was too small or `in` was malformed.
size_t base64DecodeCore(const char* in, size_t inLen, uint8_t* out, size_t outCap);

// ============================================================================
// Streaming decode
// ============================================================================
//
// Same algorithm as base64DecodeCore, but split into init + repeatable update
// calls so base64 text arriving in arbitrary, network-dictated chunks (e.g.
// bytes trickling in off a TLS socket) can be decoded incrementally without
// ever holding the whole encoded string in memory. base64DecodeCore itself is
// implemented as init() + one update() call over this same state machine.

// Accumulates up to 6 pending bits between update() calls -- a base64
// character encodes 6 bits, a byte needs 8, so a stream position isn't
// always sitting on a whole-byte boundary between calls.
typedef struct {
    uint32_t bitBuffer;
    int bits;
} Base64DecodeStream;

void base64DecodeStreamInit(Base64DecodeStream* state);

// Decodes as many complete bytes as possible from `in` (inLen characters),
// combined with any bits carried over from a previous call, writing to `out`.
// `out` must have room for at least base64DecodedLen(inLen) bytes. Returns
// the number of bytes written, or 0 if `out` was too small. Safe to call
// repeatedly across an arbitrarily split stream, including splits that land
// mid-character-group -- leftover bits simply carry into the next call.
size_t base64DecodeStreamUpdate(Base64DecodeStream* state, const char* in, size_t inLen, uint8_t* out, size_t outCap);
