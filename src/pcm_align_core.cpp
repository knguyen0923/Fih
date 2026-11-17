#include "pcm_align_core.h"
#include <string.h>

size_t pcmAlignPush(PcmAligner* a, const uint8_t* in, size_t inLen, uint8_t* out) {
    size_t n = 0;
    if (a->hasCarry) {
        out[n++] = a->carry;
        a->hasCarry = false;
    }
    if (inLen > 0) {
        memcpy(out + n, in, inLen);
        n += inLen;
    }
    if (n & 1) {
        a->carry = out[n - 1];
        a->hasCarry = true;
        n--;
    }
    return n;
}
