// ============================================================================
// test_main.cpp — unit tests for the pure (hardware-independent) logic
// ============================================================================
//
// Run with: pio test -e native
//
// These tests cover base64_core.h/.cpp (the base64 codec) and wav.h/.cpp (WAV
// header construction) -- the two pieces of this firmware with no Arduino or
// ESP-IDF dependency, and therefore the only pieces that can run as plain,
// fast, host-machine tests without an actual ESP32 attached.
//
// Everything else (I2S capture/playback, WiFi, the Gemini HTTPS calls) can
// only be meaningfully exercised on real hardware -- see the README's "Build
// phases" section and CLAUDE.md's checklist for how that gets verified.

#include <unity.h>
#include <string.h>
#include "base64_core.h"
#include "wav.h"

void setUp(void) {}
void tearDown(void) {}

// ----------------------------------------------------------------------------
// base64_core
// ----------------------------------------------------------------------------

void test_base64_encode_empty(void) {
    char out[8];
    size_t written = base64EncodeCore(nullptr, 0, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)written);
}

// "M" / "Ma" / "Man" -> "TQ==" / "TWE=" / "TWFu" are the textbook base64
// vectors (the classic Wikipedia example), covering all three padding cases:
// one leftover byte, two leftover bytes, and an exact multiple of three.
void test_base64_encode_known_vectors(void) {
    char out[16];

    size_t n1 = base64EncodeCore((const uint8_t*)"M", 1, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)n1);
    TEST_ASSERT_EQUAL_MEMORY("TQ==", out, 4);

    size_t n2 = base64EncodeCore((const uint8_t*)"Ma", 2, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)n2);
    TEST_ASSERT_EQUAL_MEMORY("TWE=", out, 4);

    size_t n3 = base64EncodeCore((const uint8_t*)"Man", 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)n3);
    TEST_ASSERT_EQUAL_MEMORY("TWFu", out, 4);
}

void test_base64_decode_known_vectors(void) {
    uint8_t out[8];

    size_t n1 = base64DecodeCore("TQ==", 4, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)n1);
    TEST_ASSERT_EQUAL_MEMORY("M", out, 1);

    size_t n2 = base64DecodeCore("TWE=", 4, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)n2);
    TEST_ASSERT_EQUAL_MEMORY("Ma", out, 2);

    size_t n3 = base64DecodeCore("TWFu", 4, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)n3);
    TEST_ASSERT_EQUAL_MEMORY("Man", out, 3);
}

// Encodes then decodes a buffer covering the full byte range (including 0x00
// and 0xFF, which are the values most likely to expose an off-by-one in a
// hand-rolled bit-shifting codec) and checks we get back exactly what went in.
// This is the same round trip the real recording -> upload -> TTS -> playback
// path relies on, just exercised directly instead of over HTTPS.
void test_base64_roundtrip_full_byte_range(void) {
    uint8_t original[256];
    for (int i = 0; i < 256; i++) original[i] = (uint8_t)i;

    char encoded[512];
    size_t encLen = base64EncodeCore(original, sizeof(original), encoded, sizeof(encoded));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)base64EncodedLen(sizeof(original)), (uint32_t)encLen);

    uint8_t decoded[256];
    size_t decLen = base64DecodeCore(encoded, encLen, decoded, sizeof(decoded));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(original), (uint32_t)decLen);
    TEST_ASSERT_EQUAL_MEMORY(original, decoded, sizeof(original));
}

// A ~470KB recording is the realistic worst case (MAX_RECORDING_SECONDS x
// SAMPLE_RATE_CAPTURE x 2 bytes, see pins.h) -- this exercises the codec at
// roughly that scale rather than only ever testing tiny buffers.
void test_base64_roundtrip_large_buffer(void) {
    static uint8_t original[470000];
    for (size_t i = 0; i < sizeof(original); i++) original[i] = (uint8_t)(i * 31 + 7);

    static char encoded[650000];
    size_t encLen = base64EncodeCore(original, sizeof(original), encoded, sizeof(encoded));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)base64EncodedLen(sizeof(original)), (uint32_t)encLen);

    static uint8_t decoded[470000];
    size_t decLen = base64DecodeCore(encoded, encLen, decoded, sizeof(decoded));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(original), (uint32_t)decLen);
    TEST_ASSERT_EQUAL_MEMORY(original, decoded, sizeof(original));
}

void test_base64_encode_output_buffer_too_small(void) {
    char out[2]; // "Man" needs 4 bytes encoded
    size_t n = base64EncodeCore((const uint8_t*)"Man", 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)n);
}

void test_base64_decode_output_buffer_too_small(void) {
    uint8_t out[1]; // "TWFu" decodes to 3 bytes
    size_t n = base64DecodeCore("TWFu", 4, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)n);
}

// Feeds `encoded` (encLen chars) through base64DecodeStreamUpdate() split
// into fixed-size pieces of `chunkSize`, concatenating the decoded output as
// it goes. Used below to check the streaming decoder gives the exact same
// result as the one-shot base64DecodeCore() regardless of where a chunk
// boundary happens to fall -- including mid-character-group splits, which
// is exactly the situation synthesizeSpeech() faces reading TTS audio off a
// TLS socket in gemini.cpp.
static size_t decodeInChunks(const char* encoded, size_t encLen, size_t chunkSize, uint8_t* out, size_t outCap) {
    Base64DecodeStream state;
    base64DecodeStreamInit(&state);

    size_t outLen = 0;
    for (size_t pos = 0; pos < encLen; pos += chunkSize) {
        size_t n = (pos + chunkSize <= encLen) ? chunkSize : (encLen - pos);
        size_t written = base64DecodeStreamUpdate(&state, encoded + pos, n, out + outLen, outCap - outLen);
        outLen += written;
    }
    return outLen;
}

// A chunk size of 1 forces every single base64 character through its own
// update() call, which is the strongest test of the cross-call bit-carry
// logic: no two characters are ever decoded in the same call.
void test_base64_decode_stream_one_byte_at_a_time(void) {
    uint8_t original[64];
    for (int i = 0; i < 64; i++) original[i] = (uint8_t)(i * 7 + 3);

    char encoded[128];
    size_t encLen = base64EncodeCore(original, sizeof(original), encoded, sizeof(encoded));

    uint8_t decoded[64];
    size_t decLen = decodeInChunks(encoded, encLen, 1, decoded, sizeof(decoded));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(original), (uint32_t)decLen);
    TEST_ASSERT_EQUAL_MEMORY(original, decoded, sizeof(original));
}

// A chunk size of 7 doesn't evenly divide into base64's 4-character groups,
// so most chunk boundaries land mid-group -- a more realistic approximation
// of arbitrary TLS read boundaries than either 1-byte or 4-byte-aligned
// splits would be.
void test_base64_decode_stream_uneven_chunks(void) {
    uint8_t original[256];
    for (int i = 0; i < 256; i++) original[i] = (uint8_t)i;

    char encoded[512];
    size_t encLen = base64EncodeCore(original, sizeof(original), encoded, sizeof(encoded));

    uint8_t decoded[256];
    size_t decLen = decodeInChunks(encoded, encLen, 7, decoded, sizeof(decoded));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(original), (uint32_t)decLen);
    TEST_ASSERT_EQUAL_MEMORY(original, decoded, sizeof(original));
}

// Same realistic-scale (~470KB) buffer as test_base64_roundtrip_large_buffer,
// but decoded via chunked streaming (in TLS-read-sized ~512-byte pieces) to
// confirm the streaming path holds up at the size synthesizeSpeech() and
// understandSpeech() actually operate at, not just at toy sizes.
void test_base64_decode_stream_large_buffer(void) {
    static uint8_t original[470000];
    for (size_t i = 0; i < sizeof(original); i++) original[i] = (uint8_t)(i * 31 + 7);

    static char encoded[650000];
    size_t encLen = base64EncodeCore(original, sizeof(original), encoded, sizeof(encoded));

    static uint8_t decoded[470000];
    size_t decLen = decodeInChunks(encoded, encLen, 512, decoded, sizeof(decoded));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(original), (uint32_t)decLen);
    TEST_ASSERT_EQUAL_MEMORY(original, decoded, sizeof(original));
}

void test_base64_decode_ignores_whitespace(void) {
    // Defensive behavior: decodeChar() returns -1 for anything outside the
    // base64 alphabet, and the decode loop skips those characters rather
    // than treating them as data.
    uint8_t out[8];
    size_t n = base64DecodeCore("TW\nFu", 5, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)n);
    TEST_ASSERT_EQUAL_MEMORY("Man", out, 3);
}

// ----------------------------------------------------------------------------
// wav
// ----------------------------------------------------------------------------

void test_wav_wrap_total_length(void) {
    uint8_t pcm[100] = {0};
    uint8_t out[200];
    size_t total = wavWrap(pcm, sizeof(pcm), out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(WAV_HEADER_SIZE + sizeof(pcm)), (uint32_t)total);
}

// Checks every field in the 44-byte header lands at the exact byte offset a
// WAV parser (or Gemini's API) expects, with the correct value for this
// project's fixed capture format (16kHz/16-bit/mono, see pins.h).
void test_wav_wrap_header_fields(void) {
    uint8_t pcm[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    uint8_t out[64];
    size_t total = wavWrap(pcm, sizeof(pcm), out, sizeof(out));
    TEST_ASSERT_TRUE(total > 0);

    TEST_ASSERT_EQUAL_MEMORY("RIFF", out + 0, 4);
    TEST_ASSERT_EQUAL_MEMORY("WAVE", out + 8, 4);
    TEST_ASSERT_EQUAL_MEMORY("fmt ", out + 12, 4);
    TEST_ASSERT_EQUAL_MEMORY("data", out + 36, 4);

    uint32_t riffChunkSize;
    memcpy(&riffChunkSize, out + 4, 4);
    TEST_ASSERT_EQUAL_UINT32(36 + sizeof(pcm), riffChunkSize);

    uint32_t fmtChunkSize;
    memcpy(&fmtChunkSize, out + 16, 4);
    TEST_ASSERT_EQUAL_UINT32(16, fmtChunkSize);

    uint16_t audioFormat;
    memcpy(&audioFormat, out + 20, 2);
    TEST_ASSERT_EQUAL_UINT16(1, audioFormat); // 1 = uncompressed PCM

    uint16_t numChannels;
    memcpy(&numChannels, out + 22, 2);
    TEST_ASSERT_EQUAL_UINT16(1, numChannels); // mono

    uint32_t sampleRate;
    memcpy(&sampleRate, out + 24, 4);
    TEST_ASSERT_EQUAL_UINT32(16000, sampleRate); // SAMPLE_RATE_CAPTURE in pins.h

    uint32_t byteRate;
    memcpy(&byteRate, out + 28, 4);
    TEST_ASSERT_EQUAL_UINT32(32000, byteRate); // sampleRate * channels * bitsPerSample/8

    uint16_t blockAlign;
    memcpy(&blockAlign, out + 32, 2);
    TEST_ASSERT_EQUAL_UINT16(2, blockAlign);

    uint16_t bitsPerSample;
    memcpy(&bitsPerSample, out + 34, 2);
    TEST_ASSERT_EQUAL_UINT16(16, bitsPerSample);

    uint32_t dataSize;
    memcpy(&dataSize, out + 40, 4);
    TEST_ASSERT_EQUAL_UINT32(sizeof(pcm), dataSize);
}

void test_wav_wrap_payload_preserved(void) {
    uint8_t pcm[6] = {10, 20, 30, 40, 50, 60};
    uint8_t out[64];
    wavWrap(pcm, sizeof(pcm), out, sizeof(out));
    TEST_ASSERT_EQUAL_MEMORY(pcm, out + WAV_HEADER_SIZE, sizeof(pcm));
}

void test_wav_wrap_zero_length_payload(void) {
    uint8_t dummy[1]; // memcpy(..., ptr, 0) needs a valid (if unused) pointer, not nullptr
    uint8_t out[64];
    size_t total = wavWrap(dummy, 0, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)WAV_HEADER_SIZE, (uint32_t)total);

    uint32_t dataSize;
    memcpy(&dataSize, out + 40, 4);
    TEST_ASSERT_EQUAL_UINT32(0, dataSize);
}

void test_wav_wrap_output_buffer_too_small(void) {
    uint8_t pcm[100] = {0};
    uint8_t out[50]; // needs WAV_HEADER_SIZE + 100 = 144
    size_t total = wavWrap(pcm, sizeof(pcm), out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)total);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_base64_encode_empty);
    RUN_TEST(test_base64_encode_known_vectors);
    RUN_TEST(test_base64_decode_known_vectors);
    RUN_TEST(test_base64_roundtrip_full_byte_range);
    RUN_TEST(test_base64_roundtrip_large_buffer);
    RUN_TEST(test_base64_encode_output_buffer_too_small);
    RUN_TEST(test_base64_decode_output_buffer_too_small);
    RUN_TEST(test_base64_decode_stream_one_byte_at_a_time);
    RUN_TEST(test_base64_decode_stream_uneven_chunks);
    RUN_TEST(test_base64_decode_stream_large_buffer);
    RUN_TEST(test_base64_decode_ignores_whitespace);

    RUN_TEST(test_wav_wrap_total_length);
    RUN_TEST(test_wav_wrap_header_fields);
    RUN_TEST(test_wav_wrap_payload_preserved);
    RUN_TEST(test_wav_wrap_zero_length_payload);
    RUN_TEST(test_wav_wrap_output_buffer_too_small);

    return UNITY_END();
}
