#include "json_scan_core.h"
#include <string.h>

static bool isJsonSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool jsonFindStringValueStart(const char* json, size_t len, const char* key, size_t* valueStart) {
    const size_t keyLen = strlen(key);
    // Shortest possible match: "k":" -- quote, key, quote, colon, quote.
    if (len < keyLen + 4) return false;

    for (size_t i = 0; i + keyLen + 2 <= len; i++) {
        if (json[i] != '"') continue;
        if (memcmp(json + i + 1, key, keyLen) != 0) continue;
        if (json[i + 1 + keyLen] != '"') continue;

        size_t p = i + keyLen + 2;
        while (p < len && isJsonSpace(json[p])) p++;
        if (p >= len || json[p] != ':') continue;
        p++;
        while (p < len && isJsonSpace(json[p])) p++;
        if (p >= len || json[p] != '"') continue;

        *valueStart = p + 1;
        return true;
    }
    return false;
}
