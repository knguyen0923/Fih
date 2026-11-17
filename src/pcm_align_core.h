#pragma once
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// pcm_align_core — keep a streamed 16-bit PCM byte stream sample-aligned
// ============================================================================
//
// Zero Arduino/ESP-IDF dependency, so it's unit-testable on a desktop.
//
// Gemini's TTS audio arrives as base64 text in arbitrary-sized network reads,
// so the decoded bytes come out in arbitrary-sized chunks -- including odd
// ones. Playback code consumes whole 16-bit samples (len / 2) and drops a
// trailing odd byte, which would leave every following sample byte-shifted
// (loud static). This carries a leftover odd byte over to the next chunk so
// every chunk handed to playback is a whole number of samples.

struct PcmAligner {
    uint8_t carry;
    bool hasCarry;
};

inline void pcmAlignerInit(PcmAligner* a) {
    a->carry = 0;
    a->hasCarry = false;
}

// Copies `in` (preceded by any carried-over byte) into `out`, stopping on an
// even byte count, and carries any final odd byte for the next call.
// `out` must have room for inLen + 1 bytes. Returns the (always even) number
// of bytes written to `out`.
size_t pcmAlignPush(PcmAligner* a, const uint8_t* in, size_t inLen, uint8_t* out);
