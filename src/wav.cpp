#include "wav.h"
#include "pins.h"
#include <string.h>

size_t wavWrap(const uint8_t* pcm, size_t pcmLen, uint8_t* out, size_t outCap) {
    if (outCap < WAV_HEADER_SIZE + pcmLen) return 0;

    // A WAV file is a "RIFF" container: a top-level RIFF chunk containing a
    // "WAVE" identifier, an "fmt " sub-chunk describing the audio format, and
    // a "data" sub-chunk holding the raw samples. All the fields below are
    // exactly what's required to describe 16-bit/16kHz/mono PCM — nothing
    // fancier is needed for what Gemini's audio-understanding endpoint expects.
    const uint32_t sampleRate = SAMPLE_RATE_CAPTURE;
    const uint16_t bitsPerSample = 16;
    const uint16_t numChannels = 1; // mono
    const uint32_t byteRate = sampleRate * numChannels * bitsPerSample / 8; // bytes/sec, for playback software that wants it
    const uint16_t blockAlign = numChannels * bitsPerSample / 8;            // bytes per single sample "frame"
    const uint32_t dataSize = (uint32_t)pcmLen;
    const uint32_t riffChunkSize = 36 + dataSize; // total file size minus 8 bytes (the "RIFF" + size fields themselves)
    const uint32_t fmtChunkSize = 16;              // fixed size of the "fmt " sub-chunk for plain PCM
    const uint16_t audioFormat = 1;                // 1 = uncompressed PCM (as opposed to compressed formats)

    uint8_t* p = out;
    memcpy(p, "RIFF", 4); p += 4;
    memcpy(p, &riffChunkSize, 4); p += 4;
    memcpy(p, "WAVE", 4); p += 4;
    memcpy(p, "fmt ", 4); p += 4;          // note the trailing space -- it's part of the 4-char chunk ID
    memcpy(p, &fmtChunkSize, 4); p += 4;
    memcpy(p, &audioFormat, 2); p += 2;
    memcpy(p, &numChannels, 2); p += 2;
    memcpy(p, &sampleRate, 4); p += 4;
    memcpy(p, &byteRate, 4); p += 4;
    memcpy(p, &blockAlign, 2); p += 2;
    memcpy(p, &bitsPerSample, 2); p += 2;
    memcpy(p, "data", 4); p += 4;
    memcpy(p, &dataSize, 4); p += 4;
    // At this point exactly WAV_HEADER_SIZE (44) bytes have been written.

    memcpy(out + WAV_HEADER_SIZE, pcm, pcmLen);
    return WAV_HEADER_SIZE + pcmLen;
}
