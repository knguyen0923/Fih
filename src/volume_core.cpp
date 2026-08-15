#include "volume_core.h"
#include <math.h>

uint8_t computeVolumeLevel(const int16_t* samples, size_t sampleCount) {
    if (samples == nullptr || sampleCount == 0) return 0;

    double sumSquares = 0.0;
    for (size_t i = 0; i < sampleCount; i++) {
        double s = (double)samples[i];
        sumSquares += s * s;
    }
    double rms = sqrt(sumSquares / (double)sampleCount);

    // Scale against 32768 (one past INT16_MAX) so a full-scale square wave --
    // the loudest a 16-bit signal can ever be -- reads as (just under) 255,
    // rather than needing an unreachable reference point.
    double scaled = (rms / 32768.0) * 255.0;
    if (scaled > 255.0) scaled = 255.0;
    if (scaled < 0.0) scaled = 0.0;
    return (uint8_t)(scaled + 0.5); // round to nearest
}
