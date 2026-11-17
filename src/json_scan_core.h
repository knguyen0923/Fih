#pragma once
#include <stddef.h>

// ============================================================================
// json_scan_core — locate a JSON string value without parsing the document
// ============================================================================
//
// Zero Arduino/ESP-IDF dependency, so it's unit-testable on a desktop (see
// test/test_native/). Used by gemini.cpp to find the `data` and `mimeType`
// fields in Gemini's TTS response while it's still streaming in.
//
// Gemini's REST responses are pretty-printed ("data": "..." -- a space after
// the colon, sometimes a newline), so a plain search for the exact bytes
// `"data":"` never matches. This skips optional whitespace on either side of
// the colon instead.

// Looks for `"key"`, optional whitespace, `:`, optional whitespace, then the
// opening quote of a string value, anywhere in json[0..len).
//   json, len   -- the text to scan (need not be NUL-terminated)
//   key         -- the field name, without quotes
//   valueStart  -- on success, set to the index of the first character AFTER
//                  the value's opening quote
// Returns true only if the opening quote was found. If the key is present
// but the text ends before the opening quote arrives (a streamed head that's
// been cut off mid-whitespace), this returns false -- the caller reads more
// and calls again.
bool jsonFindStringValueStart(const char* json, size_t len, const char* key, size_t* valueStart);
