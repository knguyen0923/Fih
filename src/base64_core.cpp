#include "base64_core.h"

// The 64 characters used to represent 6 bits of data each. Index 0-25 are
// uppercase letters, 26-51 lowercase, 52-61 digits, then '+' and '/'.
static const char ENCODE_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// The reverse lookup: given a base64 character, what 6-bit value (0-63) does
// it represent? Returns -1 for characters that aren't part of the alphabet
// (padding '=', whitespace, newlines, etc.) so the caller can skip them.
static inline int8_t decodeChar(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

size_t base64EncodedLen(size_t len) {
    return ((len + 2) / 3) * 4;
}

size_t base64DecodedLen(size_t encodedLen) {
    return (encodedLen / 4) * 3 + 3;
}

size_t base64EncodeCore(const uint8_t* data, size_t len, char* out, size_t outCap) {
    size_t needed = base64EncodedLen(len);
    if (outCap < needed) return 0;

    size_t o = 0;
    size_t i = 0;

    // Main loop: base64 works on 3 input bytes (24 bits) at a time, which
    // split evenly into four 6-bit groups -> four output characters.
    while (i + 3 <= len) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out[o++] = ENCODE_TABLE[(n >> 18) & 0x3F]; // top 6 bits
        out[o++] = ENCODE_TABLE[(n >> 12) & 0x3F];
        out[o++] = ENCODE_TABLE[(n >> 6) & 0x3F];
        out[o++] = ENCODE_TABLE[n & 0x3F];          // bottom 6 bits
        i += 3;
    }

    // Handle the 1 or 2 leftover bytes at the end, if len isn't a multiple of
    // 3. Padding with '=' tells the decoder how many of the last 4 characters
    // are "real" data vs filler.
    size_t remaining = len - i;
    if (remaining == 1) {
        uint32_t n = (uint32_t)data[i] << 16;
        out[o++] = ENCODE_TABLE[(n >> 18) & 0x3F];
        out[o++] = ENCODE_TABLE[(n >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (remaining == 2) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        out[o++] = ENCODE_TABLE[(n >> 18) & 0x3F];
        out[o++] = ENCODE_TABLE[(n >> 12) & 0x3F];
        out[o++] = ENCODE_TABLE[(n >> 6) & 0x3F];
        out[o++] = '=';
    }

    return o;
}

size_t base64DecodeCore(const char* in, size_t inLen, uint8_t* out, size_t outCap) {
    Base64DecodeStream state;
    base64DecodeStreamInit(&state);
    return base64DecodeStreamUpdate(&state, in, inLen, out, outCap);
}

void base64DecodeStreamInit(Base64DecodeStream* state) {
    state->bitBuffer = 0;
    state->bits = 0;
}

size_t base64DecodeStreamUpdate(Base64DecodeStream* state, const char* in, size_t inLen, uint8_t* out, size_t outCap) {
    // '=' padding characters (and any whitespace/newlines) fall outside the
    // base64 alphabet, so decodeChar() returns -1 for them and they're
    // skipped below just like any other non-data byte -- no separate
    // padding-trim step is needed, which is what makes this safe to call on
    // an arbitrary, mid-group slice of a larger stream.
    size_t outLen = 0;

    for (size_t i = 0; i < inLen; i++) {
        int8_t val = decodeChar(in[i]);
        if (val < 0) continue; // skip whitespace/newlines/padding defensively

        // Shift in 6 new bits from this character, into locals rather than
        // `state` directly -- see the capacity check below for why.
        uint32_t nextBitBuffer = (state->bitBuffer << 6) | (uint32_t)val;
        int nextBits = state->bits + 6;

        // Once we've accumulated a full byte (8 bits) worth, peel it off the
        // top of the buffer and emit it.
        if (nextBits >= 8) {
            if (outLen >= outCap) {
                // Caller's buffer is full. Stop here WITHOUT committing this
                // character's bits to `state` (the contract is that `out`
                // holds at least base64DecodedLen(inLen) bytes, so a
                // well-behaved caller never actually reaches this -- but the
                // old version updated `state` unconditionally before this
                // check, so a caller that ever did violate the contract
                // would lose the in-flight byte AND leave every future
                // update() call on this stream permanently bit-misaligned,
                // silently corrupting the rest of the decode instead of just
                // this one chunk. Breaking here instead keeps `state` at
                // exactly the last successfully-committed position, so
                // decoding can pick back up correctly once the caller passes
                // a large enough buffer.
                break;
            }
            nextBits -= 8;
            out[outLen++] = (uint8_t)((nextBitBuffer >> nextBits) & 0xFF);
        }

        state->bitBuffer = nextBitBuffer;
        state->bits = nextBits;
    }

    return outLen;
}
