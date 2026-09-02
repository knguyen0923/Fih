#include "gemini.h"
#include "config.h"
#include "base64_core.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Stream.h>
#include <ArduinoJson.h>

// All Gemini generateContent calls hit the same host, differing only by which
// model name follows it (see config.h for GEMINI_TEXT_MODEL/GEMINI_TTS_MODEL).
static const char* GEMINI_HOST = "https://generativelanguage.googleapis.com/v1beta/models/";

// Fixed instruction sent alongside the recorded audio in understandSpeech().
// This is deliberately a plain, punctuation-free literal we control ourselves
// (as opposed to user-generated text), so it's safe to splice directly into a
// hand-built JSON string without worrying about characters that would need
// escaping.
static const char* SPEECH_PROMPT = "Respond conversationally and briefly to this spoken question.";

// How long to wait before retrying once after an HTTP 429 (rate limited)
// response. A single fixed-delay retry is enough for a personal device that
// isn't hammering the API constantly — see the free-tier notes in the README.
static const unsigned long RETRY_DELAY_MS = 2000;

// Finds `"key":"..."` inside a JSON response string and extracts the raw
// (still-escaped) text between the quotes, without doing a full JSON parse.
//
// Used to pull the small `mimeType` field out of the (bounded) head of the
// TTS response -- see streamTtsResponse() below. Base64 text is guaranteed to
// only ever contain [A-Za-z0-9+/=] -- there's nothing in it that JSON would
// ever need to escape -- so this substring approach is also how the (much
// larger) `data` field is located, just via a streaming scan instead of this
// whole-string helper.
static bool extractJsonStringField(const String& json, const char* key, String& out) {
    String needle = String("\"") + key + "\":\"";
    int start = json.indexOf(needle);
    if (start < 0) return false;
    start += needle.length();

    // Scan forward to the closing quote, skipping over any escaped character
    // (a backslash followed by anything) so an escaped quote inside the value
    // doesn't get mistaken for the end of the string.
    int end = start;
    while (end < (int)json.length()) {
        if (json[end] == '\\') { end += 2; continue; }
        if (json[end] == '"') break;
        end++;
    }
    if (end >= (int)json.length()) return false; // ran off the end without finding a closing quote

    out = json.substring(start, end);
    return true;
}

// Shared HTTP plumbing for small-body calls: opens a TLS connection, POSTs
// the given JSON body with the API key attached, and returns the raw response
// body as text. Retries once on a 429 (rate limit) response.
//
// Only used where both the request and response are small enough to hold
// in memory whole (textOnlyQuery(); understandSpeech()'s much larger audio
// upload uses postStreamed() below instead, and synthesizeSpeech()'s much
// larger audio response is read incrementally via its own HTTPClient call --
// see streamTtsResponse()).
static bool postJson(const String& url, const String& body, String& responseOut) {
    for (int attempt = 0; attempt < 2; attempt++) {
        WiFiClientSecure client;
        // setInsecure() skips TLS certificate validation entirely. This is a
        // deliberate simplicity tradeoff acceptable for a personal device (see
        // the security notes in the README) -- swap in a pinned Google root CA
        // here instead if you want stricter validation.
        client.setInsecure();

        HTTPClient https;
        if (!https.begin(client, url)) return false;
        https.addHeader("Content-Type", "application/json");
        // The API key travels as a request header rather than a URL query
        // parameter -- both are accepted by Gemini's API, but the header form
        // keeps the key out of any request logging that only captures URLs.
        https.addHeader("x-goog-api-key", GEMINI_API_KEY);

        int code = https.POST((uint8_t*)body.c_str(), body.length());
        if (code == 200) {
            responseOut = https.getString();
            https.end();
            return true;
        }
        https.end();

        if (code == 429 && attempt == 0) {
            delay(RETRY_DELAY_MS);
            continue; // second and final attempt
        }
        Serial.printf("Gemini request failed, HTTP %d\n", code);
        return false;
    }
    return false;
}

// ============================================================================
// Streaming upload body for understandSpeech()
// ============================================================================
//
// A read-only Arduino Stream that lazily base64-encodes a WAV buffer as it's
// read, so HTTPClient's streaming-body upload (sendRequest(type, Stream*,
// size)) never needs the ~1.3x-larger encoded copy of it in memory at once --
// this board has no PSRAM to spare for that. Serves three segments in order:
//   [prefix literal]  [base64(wavData)]  [suffix literal]
// The prefix/suffix are small JSON literals the caller already fully built;
// wavData's length is known upfront (recording already finished by the time
// understandSpeech() is called), so the total served length -- and therefore
// an exact Content-Length -- can be computed without ever encoding it all at
// once.
class Base64AudioBodyStream : public Stream {
public:
    Base64AudioBodyStream(const char* prefix, size_t prefixLen,
                           const uint8_t* wavData, size_t wavLen,
                           const char* suffix, size_t suffixLen)
        : _prefix(prefix), _prefixLen(prefixLen),
          _wavData(wavData), _wavLen(wavLen),
          _suffix(suffix), _suffixLen(suffixLen) {
        _totalLen = prefixLen + base64EncodedLen(wavLen) + suffixLen;
        reset();
    }

    size_t totalLength() const { return _totalLen; }

    // Rewinds back to the start -- needed if a request has to be retried
    // (e.g. after a 429) since the stream is consumed by the first attempt.
    void reset() {
        _prefixPos = 0;
        _wavPos = 0;
        _suffixPos = 0;
        _stageLen = 0;
        _stageOff = 0;
        _remaining = _totalLen;
    }

    int available() override { return (int)_remaining; }
    int peek() override { return -1; } // not needed by HTTPClient's streamed-upload path
    int read() override {
        uint8_t b;
        return readBytes(&b, 1) == 1 ? (int)b : -1;
    }
    size_t write(uint8_t) override { return 0; } // read-only stream, never written to

    // HTTPClient's streamed upload calls readBytes() in reasonably large
    // chunks -- this is the path that actually matters for throughput; read()
    // above only exists to satisfy Stream's interface.
    size_t readBytes(uint8_t* buf, size_t len) override {
        size_t written = 0;
        while (written < len && _remaining > 0) {
            if (_prefixPos < _prefixLen) {
                size_t chunk = min(len - written, _prefixLen - _prefixPos);
                memcpy(buf + written, _prefix + _prefixPos, chunk);
                _prefixPos += chunk; written += chunk; _remaining -= chunk;
                continue;
            }
            if (_wavPos < _wavLen || _stageOff < _stageLen) {
                // Refill the encoded staging buffer from a fixed 768-byte
                // (multiple-of-3) raw chunk whenever it's been fully drained
                // -- 768/3 divides evenly, so each refill encodes with no
                // leftover bits to carry, regardless of how small the
                // caller's requested `len` is.
                if (_stageOff >= _stageLen) {
                    size_t rawChunk = min((size_t)RAW_CHUNK, _wavLen - _wavPos);
                    _stageLen = base64EncodeCore(_wavData + _wavPos, rawChunk, _stage, STAGE_CAP);
                    _wavPos += rawChunk;
                    _stageOff = 0;
                }
                size_t chunk = min(len - written, _stageLen - _stageOff);
                memcpy(buf + written, _stage + _stageOff, chunk);
                _stageOff += chunk; written += chunk; _remaining -= chunk;
                continue;
            }
            if (_suffixPos < _suffixLen) {
                size_t chunk = min(len - written, _suffixLen - _suffixPos);
                memcpy(buf + written, _suffix + _suffixPos, chunk);
                _suffixPos += chunk; written += chunk; _remaining -= chunk;
                continue;
            }
            break; // nothing left in any segment
        }
        return written;
    }

private:
    static const size_t RAW_CHUNK = 768;  // multiple of 3 -- encodes with no leftover bits
    static const size_t STAGE_CAP = 1024; // base64EncodedLen(RAW_CHUNK)

    const char* _prefix; size_t _prefixLen, _prefixPos;
    const uint8_t* _wavData; size_t _wavLen, _wavPos;
    const char* _suffix; size_t _suffixLen, _suffixPos;
    char _stage[STAGE_CAP]; size_t _stageLen, _stageOff;
    size_t _totalLen, _remaining;
};

// Same TLS/retry plumbing as postJson(), but sends a pre-sized Stream body
// instead of a String -- see Base64AudioBodyStream above.
static bool postStreamed(const String& url, Base64AudioBodyStream& body, String& responseOut) {
    for (int attempt = 0; attempt < 2; attempt++) {
        WiFiClientSecure client;
        client.setInsecure();

        HTTPClient https;
        if (!https.begin(client, url)) return false;
        https.addHeader("Content-Type", "application/json");
        https.addHeader("x-goog-api-key", GEMINI_API_KEY);

        int code = https.sendRequest("POST", &body, body.totalLength());
        if (code == 200) {
            responseOut = https.getString();
            https.end();
            return true;
        }
        https.end();

        if (code == 429 && attempt == 0) {
            delay(RETRY_DELAY_MS);
            body.reset();
            continue; // second and final attempt
        }
        Serial.printf("Gemini request failed, HTTP %d\n", code);
        return false;
    }
    return false;
}

bool understandSpeech(const uint8_t* wavData, size_t wavLen, String& replyTextOut) {
    // Request body is built as prefix/suffix literals around the audio,
    // rather than one big concatenated string -- Base64AudioBodyStream
    // streams the (large) base64-encoded audio between them directly to the
    // socket, so the encoded copy of it never exists in memory all at once.
    //
    // Gemini's REST JSON uses camelCase field names (inlineData, mimeType),
    // not snake_case -- worth calling out since a lot of other Google API
    // documentation defaults to snake_case.
    String prefix = "{\"contents\":[{\"parts\":[{\"text\":\"";
    prefix += SPEECH_PROMPT;
    prefix += "\"},{\"inlineData\":{\"mimeType\":\"audio/wav\",\"data\":\"";

    String suffix = "\"}}]}],\"generationConfig\":{\"maxOutputTokens\":";
    suffix += String(GEMINI_MAX_OUTPUT_TOKENS);
    suffix += "}}";

    Base64AudioBodyStream body(prefix.c_str(), prefix.length(), wavData, wavLen, suffix.c_str(), suffix.length());

    String url = String(GEMINI_HOST) + GEMINI_TEXT_MODEL + ":generateContent";
    String response;
    bool ok = postStreamed(url, body, response);
    if (!ok) return false;

    // The response is small (bounded by GEMINI_MAX_OUTPUT_TOKENS), so a full
    // ArduinoJson parse here is cheap and gets us correct handling of any
    // escaped characters in Gemini's natural-language reply (quotes,
    // newlines, unicode, etc.) that a manual string search would get wrong.
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, response);
    if (err) {
        Serial.printf("Failed to parse Gemini response: %s\n", err.c_str());
        return false;
    }

    // Response shape: { "candidates": [ { "content": { "parts": [ { "text": "..." } ] } } ] }
    const char* text = doc["candidates"][0]["content"]["parts"][0]["text"];
    if (!text) return false;
    replyTextOut = text;
    return true;
}

bool textOnlyQuery(const String& prompt, String& replyTextOut) {
    // Same shape as understandSpeech()'s request, just without the inlineData
    // audio part. Built via ArduinoJson (rather than hand-concatenated like
    // understandSpeech()'s body) since `prompt` here is arbitrary text that
    // may need JSON escaping, and this path is only used for one-off manual
    // testing where the extra safety margin costs nothing.
    JsonDocument doc;
    JsonArray contents = doc["contents"].to<JsonArray>();
    JsonObject content0 = contents.add<JsonObject>();
    JsonArray parts = content0["parts"].to<JsonArray>();
    parts.add<JsonObject>()["text"] = prompt;

    String body;
    serializeJson(doc, body);

    String url = String(GEMINI_HOST) + GEMINI_TEXT_MODEL + ":generateContent";
    String response;
    if (!postJson(url, body, response)) return false;

    JsonDocument respDoc;
    DeserializationError err = deserializeJson(respDoc, response);
    if (err) {
        Serial.printf("Failed to parse Gemini response: %s\n", err.c_str());
        return false;
    }

    const char* text = respDoc["candidates"][0]["content"]["parts"][0]["text"];
    if (!text) return false;
    replyTextOut = text;
    return true;
}

// Feeds base64 text into the streaming decoder up to (but not including) the
// first quote -- base64 text never contains '"' or '\', so a bare quote
// always marks the end of the JSON string field it's inside, with no need to
// handle escaping the way extractJsonStringField() does for arbitrary text.
// Returns true if that closing quote was found in this call (i.e. the `data`
// field is now fully consumed).
static bool feedBase64UntilQuote(Base64DecodeStream* decodeState, const char* data, size_t len,
                                  void (*onPcmChunk)(const uint8_t*, size_t)) {
    size_t i = 0;
    while (i < len && data[i] != '"') i++;

    if (i > 0) {
        uint8_t pcmScratch[400]; // room for base64DecodedLen() of one read's worth of input (<= 512 chars)
        size_t outLen = base64DecodeStreamUpdate(decodeState, data, i, pcmScratch, sizeof(pcmScratch));
        if (outLen > 0) onPcmChunk(pcmScratch, outLen);
    }
    return i < len;
}

// How long streamTtsResponse() will wait, in total, for Gemini to finish
// sending the TTS reply body before giving up. Each individual read below is
// already bounded by the underlying WiFiClientSecure's own read timeout
// (Stream::readBytes()/readStringUntil() give up and return after that many
// ms with whatever they've got, ~30s by default) -- but that alone doesn't
// cap how long a series of many small-but-nonzero reads can keep the loops
// below spinning for if the server just trickles data slowly. This is the
// actual wall-clock ceiling, matching the spirit of waitForWiFi()'s 15s
// timeout in main.cpp (added for the same reason: an unbounded wait here
// would hang the device indefinitely with no error, no LED change, and no
// recovery short of a power cycle).
static const unsigned long TTS_STREAM_TIMEOUT_MS = 20000;

// Tracks state for stripping HTTP chunked-transfer-encoding framing
// ("<hex-size>\r\n<data>\r\n", repeated, ended by a zero-size chunk) off a
// raw response stream. HTTPClient::getStream() hands back the transport
// socket completely unprocessed -- only HTTPClient's own writeToStream()/
// getString() do dechunking internally, and neither fits this function's
// need to interleave a wall-clock deadline check between reads (see
// TTS_STREAM_TIMEOUT_MS) -- so this reimplements just enough of it by hand.
// Left unstripped, chunk-size hex digits and CRLF framing would get scanned
// as if they were response bytes: at best that desyncs the "data" needle
// search below, and at worst -- since hex digits are themselves valid base64
// characters -- any that land inside the audio payload get silently decoded
// as bogus PCM instead of being recognized as framing, corrupting playback.
struct ChunkedReadState {
    bool chunked;
    uint32_t chunkRemaining;
    bool ended;
};

// Reads up to `len` bytes of actual response payload from `stream` into
// `out`, transparently stripping chunk framing when `state->chunked` is set.
// Returns 0 if nothing was available within this call's own read timeout
// (the caller's surrounding loop decides whether to retry or give up -- see
// the deadline/connected() checks around each call site below), or once a
// chunked body's terminating zero-size chunk has been seen.
static size_t readDechunked(WiFiClient& stream, ChunkedReadState* state, uint8_t* out, size_t len) {
    if (!state->chunked) {
        return stream.readBytes(out, len);
    }
    if (state->ended) return 0;
    if (state->chunkRemaining == 0) {
        String line = stream.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) return 0; // chunk-size line hasn't fully arrived yet
        state->chunkRemaining = (uint32_t)strtol(line.c_str(), nullptr, 16);
        if (state->chunkRemaining == 0) {
            state->ended = true;
            return 0;
        }
    }
    size_t want = min((size_t)state->chunkRemaining, len);
    size_t n = stream.readBytes(out, want);
    state->chunkRemaining -= n;
    if (n > 0 && state->chunkRemaining == 0) {
        char crlf[2];
        stream.readBytes((uint8_t*)crlf, 2); // trailing CRLF after each chunk's data
    }
    return n;
}

// Reads Gemini's TTS response incrementally instead of buffering the whole
// (potentially hundreds-of-KB) body first -- this board has no PSRAM to hold
// a reply-sized buffer in. Response shape:
//   { "candidates": [ { "content": { "parts": [ { "inlineData": {
//       "mimeType": "...", "data": "BASE64..." } } ] } } ] }
// Everything before "data" is small, fixed JSON metadata, so it's accumulated
// into a bounded head buffer (cheap to re-scan for the marker on every read);
// once "data" is found, its base64 text -- the large part -- is decoded and
// handed to onPcmChunk() a chunk at a time as it streams in.
static bool streamTtsResponse(HTTPClient& https, void (*onPcmChunk)(const uint8_t*, size_t)) {
    WiFiClient& stream = https.getStream();
    // Gemini sends either a Content-Length header or chunked encoding, never
    // both -- no Content-Length (getSize() < 0) means chunked. This response
    // is large and dynamically generated, exactly the case chunked encoding
    // exists for, so it's worth handling rather than assuming Content-Length.
    ChunkedReadState chunkState{ https.getSize() < 0, 0, false };

    static const size_t HEAD_CAP = 1024;
    char head[HEAD_CAP];
    size_t headLen = 0;
    int dataStart = -1;

    static const char* DATA_NEEDLE = "\"data\":\"";
    static const size_t DATA_NEEDLE_LEN = 8;

    uint8_t readBuf[512];
    unsigned long deadline = millis() + TTS_STREAM_TIMEOUT_MS;

    while (headLen < HEAD_CAP && dataStart < 0) {
        if (millis() > deadline) {
            Serial.println("TTS response: timed out waiting for \"data\" field");
            return false;
        }
        size_t want = min(sizeof(readBuf), HEAD_CAP - headLen);
        size_t n = readDechunked(stream, &chunkState, readBuf, want);
        if (n == 0) {
            if (!stream.connected() && !stream.available()) break;
            continue;
        }
        memcpy(head + headLen, readBuf, n);
        headLen += n;

        for (size_t i = 0; i + DATA_NEEDLE_LEN <= headLen; i++) {
            if (memcmp(head + i, DATA_NEEDLE, DATA_NEEDLE_LEN) == 0) {
                dataStart = (int)(i + DATA_NEEDLE_LEN);
                break;
            }
        }
    }

    if (dataStart < 0) {
        Serial.println("TTS response: \"data\" field not found");
        return false;
    }

    // Logged so the actual returned format can be spot-checked (see README)
    // against what audioPlayFromBuffer() assumes (raw 16-bit/24kHz/mono PCM,
    // no WAV wrapper) rather than only trusting that assumption blindly.
    String headStr(head, headLen);
    String mimeType;
    if (extractJsonStringField(headStr, "mimeType", mimeType)) {
        Serial.printf("TTS response mimeType: %s\n", mimeType.c_str());
    }

    Base64DecodeStream decodeState;
    base64DecodeStreamInit(&decodeState);

    if (feedBase64UntilQuote(&decodeState, head + dataStart, headLen - (size_t)dataStart, onPcmChunk)) {
        return true;
    }

    while (stream.connected() || stream.available()) {
        if (millis() > deadline) {
            Serial.println("TTS response: timed out mid-stream");
            return false;
        }
        size_t n = readDechunked(stream, &chunkState, readBuf, sizeof(readBuf));
        if (n == 0) {
            if (!stream.connected() || chunkState.ended) break;
            continue;
        }
        if (feedBase64UntilQuote(&decodeState, (const char*)readBuf, n, onPcmChunk)) {
            return true;
        }
    }
    return false; // connection closed before the data field's closing quote arrived
}

bool synthesizeSpeech(const String& text, void (*onPcmChunk)(const uint8_t* pcm, size_t len)) {
    // Request shape:
    // {
    //   "contents": [{"parts": [{"text": "..."}]}],
    //   "generationConfig": {
    //     "responseModalities": ["AUDIO"],
    //     "speechConfig": {"voiceConfig": {"prebuiltVoiceConfig": {"voiceName": "..."}}}
    //   }
    // }
    // Built via ArduinoJson (not hand-concatenated) because `text` is
    // Gemini's own natural-language reply and may contain quotes or other
    // characters that need proper JSON escaping. This request body is small
    // (bounded by GEMINI_MAX_OUTPUT_TOKENS) so, unlike the response, it's
    // fine to build and send in one shot.
    JsonDocument doc;
    JsonArray contents = doc["contents"].to<JsonArray>();
    JsonObject content0 = contents.add<JsonObject>();
    JsonArray parts = content0["parts"].to<JsonArray>();
    parts.add<JsonObject>()["text"] = text;

    JsonObject genConfig = doc["generationConfig"].to<JsonObject>();
    // responseModalities tells Gemini we want audio back, not text.
    genConfig["responseModalities"].to<JsonArray>().add("AUDIO");
    genConfig["speechConfig"]["voiceConfig"]["prebuiltVoiceConfig"]["voiceName"] = GEMINI_TTS_VOICE;

    String body;
    serializeJson(doc, body);

    String url = String(GEMINI_HOST) + GEMINI_TTS_MODEL + ":generateContent";

    for (int attempt = 0; attempt < 2; attempt++) {
        WiFiClientSecure client;
        client.setInsecure();

        HTTPClient https;
        if (!https.begin(client, url)) return false;
        https.addHeader("Content-Type", "application/json");
        https.addHeader("x-goog-api-key", GEMINI_API_KEY);

        int code = https.POST((uint8_t*)body.c_str(), body.length());
        if (code == 200) {
            bool ok = streamTtsResponse(https, onPcmChunk);
            https.end();
            return ok;
        }
        https.end();

        if (code == 429 && attempt == 0) {
            delay(RETRY_DELAY_MS);
            continue; // second and final attempt
        }
        Serial.printf("Gemini request failed, HTTP %d\n", code);
        return false;
    }
    return false;
}
