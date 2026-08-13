#include "base64.h"
#include <esp_heap_caps.h>

String base64Encode(const uint8_t* data, size_t len) {
    size_t needed = base64EncodedLen(len);

    // A large recording can produce a base64 string in the hundreds of KB.
    // Rather than growing an Arduino String character-by-character (which
    // risks repeated reallocation on the PSRAM heap), encode into a scratch
    // PSRAM buffer via the pure core algorithm, then build the String from it
    // in one shot.
    char* scratch = (char*)heap_caps_malloc(needed > 0 ? needed : 1, MALLOC_CAP_SPIRAM);
    if (!scratch) return String();

    size_t written = base64EncodeCore(data, len, scratch, needed);
    String out(scratch, written);
    heap_caps_free(scratch);
    return out;
}

size_t base64Decode(const String& in, uint8_t* out, size_t outCap) {
    return base64DecodeCore(in.c_str(), in.length(), out, outCap);
}
