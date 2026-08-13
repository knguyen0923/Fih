#include "gemini.h"
#include "config.h"
#include "base64.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
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
// This is used specifically for pulling the TTS response's base64 `data`
// field back out. Base64 text is guaranteed to only ever contain
// [A-Za-z0-9+/=] -- there's nothing in it that JSON would ever need to escape
// -- so a full ArduinoJson parse (which would allocate a second copy of a
// payload that can be hundreds of KB) is unnecessary overhead. A simple
// substring search does the same job for a fraction of the memory.
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

// Shared HTTP plumbing for both API calls: opens a TLS connection, POSTs the
// given JSON body with the API key attached, and returns the raw response
// body as text. Retries once on a 429 (rate limit) response.
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

bool understandSpeech(const uint8_t* wavData, size_t wavLen, String& replyTextOut) {
    // Base64-encode the WAV audio first -- this can be a large string (a 15s
    // recording is roughly ~625KB once encoded), so it's built once here and
    // freed as soon as it's been copied into the request body below.
    String audioB64 = base64Encode(wavData, wavLen);

    // The request body is hand-built as a plain string rather than through
    // ArduinoJson. For a payload this large, ArduinoJson would need to hold
    // its own internal copy of the entire base64 string in addition to the
    // one we already have, roughly doubling peak memory use for no benefit --
    // string concatenation is fine since every piece being spliced in is
    // either a literal we control or pure base64 text that never needs
    // escaping.
    //
    // Gemini's REST JSON uses camelCase field names (inlineData, mimeType),
    // not snake_case -- worth calling out since a lot of other Google API
    // documentation defaults to snake_case.
    String body;
    body.reserve(audioB64.length() + 300); // avoid repeated reallocation while appending below
    body += "{\"contents\":[{\"parts\":[{\"text\":\"";
    body += SPEECH_PROMPT;
    body += "\"},{\"inlineData\":{\"mimeType\":\"audio/wav\",\"data\":\"";
    body += audioB64;
    body += "\"}}]}],\"generationConfig\":{\"maxOutputTokens\":";
    body += String(GEMINI_MAX_OUTPUT_TOKENS);
    body += "}}";
    audioB64 = String(); // free the large base64 copy now that it's been copied into `body`

    String url = String(GEMINI_HOST) + GEMINI_TEXT_MODEL + ":generateContent";
    String response;
    bool ok = postJson(url, body, response);
    body = String(); // free the request body before parsing the response
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

bool synthesizeSpeech(const String& text, uint8_t* pcmOut, size_t& pcmLenOut, size_t pcmCap) {
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
    // characters that need proper JSON escaping.
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
    String response;
    bool ok = postJson(url, body, response);
    body = String();
    if (!ok) return false;

    // Logged so the actual returned format can be spot-checked (see README)
    // against what audioPlayFromBuffer() assumes (raw 16-bit/24kHz/mono PCM,
    // no WAV wrapper) rather than only trusting that assumption blindly.
    // Response shape: { "candidates": [ { "content": { "parts": [ { "inlineData": { "mimeType": "...", "data": "..." } } ] } } ] }
    String mimeType;
    if (extractJsonStringField(response, "mimeType", mimeType)) {
        Serial.printf("TTS response mimeType: %s\n", mimeType.c_str());
    }

    // See extractJsonStringField()'s comment above for why this skips
    // ArduinoJson: `data` here can be hundreds of KB of pure base64 text.
    String audioB64;
    if (!extractJsonStringField(response, "data", audioB64)) return false;
    response = String(); // free the (potentially large) response text before decoding

    pcmLenOut = base64Decode(audioB64, pcmOut, pcmCap);
    return pcmLenOut > 0;
}
