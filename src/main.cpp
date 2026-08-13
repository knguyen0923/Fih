// ============================================================================
// main.cpp — top-level state machine
// ============================================================================
//
// This is the orchestration layer that ties together audio.h (mic/speaker),
// gemini.h (the two API calls), and pins.h (hardware wiring) into the actual
// push-to-talk behavior:
//
//   IDLE -> (button pressed) -> RECORDING -> (button released) ->
//   UPLOADING -> PROCESSING -> SYNTHESIZING -> PLAYING -> back to IDLE
//
// The interesting bit to understand up front: because this whole design makes
// blocking, sequential HTTPS calls (no persistent connection, no background
// tasks), the UPLOADING/PROCESSING/SYNTHESIZING/PLAYING states don't each get
// their own pass through loop() the way IDLE and RECORDING do. Instead,
// processInteraction() runs all four of them back-to-back in one go, only
// updating `state` (and the LED) as a way to report progress -- the actual
// waiting happens inside the blocking calls to understandSpeech(),
// synthesizeSpeech(), and audioPlayFromBuffer().

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include "pins.h"
#include "config.h"
#include "audio.h"
#include "gemini.h"

enum State { IDLE, RECORDING, UPLOADING, PROCESSING, SYNTHESIZING, PLAYING, FATAL_ERROR };

static State state = IDLE;

// Worst-case size of a recording: MAX_RECORDING_SECONDS worth of 16-bit
// samples at SAMPLE_RATE_CAPTURE. This bounds how big the PSRAM allocations
// below need to be, and is the hard ceiling the RECORDING loop enforces so a
// stuck/taped-down button can't grow memory use without limit.
static const size_t MAX_PCM_BYTES = (size_t)MAX_RECORDING_SECONDS * SAMPLE_RATE_CAPTURE * sizeof(int16_t);
static const size_t MAX_WAV_BYTES = MAX_PCM_BYTES + WAV_HEADER_SIZE;
// Bounds how much decoded TTS audio can be held at once. 300KB comfortably
// covers a reply of a few sentences at 24kHz/16-bit/mono -- see the memory
// budget discussion in the project's original planning doc for the full math.
static const size_t MAX_TTS_PCM_BYTES = 300 * 1024;

// Three PSRAM buffers, allocated once in setup() and reused for every
// interaction (rather than allocating/freeing per press, which would risk
// PSRAM fragmentation over a long uptime):
static uint8_t* pcmBuf = nullptr; // raw mic recording -- actually just points partway into wavBuf, see setup()
static uint8_t* wavBuf = nullptr; // WAV header + pcmBuf's data, contiguous in one allocation
static uint8_t* ttsBuf = nullptr; // decoded TTS playback PCM, filled by synthesizeSpeech()

// --- Button debouncing ---
// A mechanical button's contacts don't cleanly go from "open" to "closed" --
// they bounce for a few milliseconds, which without debouncing would look
// like several rapid presses instead of one clean press. The fix: only trust
// a change in the raw reading once it's stayed the same for DEBOUNCE_MS.
static const unsigned long DEBOUNCE_MS = 30;
static bool lastButtonRaw = false;   // the most recent raw (un-debounced) reading
static bool buttonStable = false;    // the debounced reading everything else uses
static unsigned long lastDebounceChange = 0; // when lastButtonRaw last changed

// Returns the current debounced button state (true = pressed). Wired
// active-low with an internal pull-up, so a LOW reading means pressed.
static bool readButtonPressed() {
    bool raw = (digitalRead(PIN_BUTTON) == LOW);
    if (raw != lastButtonRaw) {
        // The raw reading just changed -- restart the debounce timer, but
        // don't trust this new value as "stable" yet.
        lastDebounceChange = millis();
        lastButtonRaw = raw;
    }
    if (millis() - lastDebounceChange > DEBOUNCE_MS) {
        // The raw reading has held steady long enough to trust it.
        buttonStable = raw;
    }
    return buttonStable;
}

// --- Status LED ---
// Gives a simple visual indicator of what the device is doing, without
// needing a display. Patterns:
//   IDLE               solid off
//   RECORDING          slow blink (300ms)
//   UPLOADING/         fast blink (120ms) -- lumped together since they're
//   PROCESSING/          all "the device is busy talking to Gemini" from the
//   SYNTHESIZING         user's perspective
//   PLAYING            solid on
//   FATAL_ERROR        very fast blink (80ms)
//
// Note the states reached only inside processInteraction() (UPLOADING through
// PLAYING) are set right before a blocking call starts, so the LED snaps to
// the new pattern's *level* immediately but won't actually animate/blink
// during the call itself (loop() isn't running to re-invoke this function).
// That's an acceptable simplification for v1 -- see the README's "Polish"
// notes for how you'd fix it with a non-blocking HTTP state machine instead.
static void updateLed() {
    static unsigned long lastToggle = 0;
    static bool ledOn = false;
    unsigned long now = millis();
    unsigned long interval;

    switch (state) {
        case IDLE:        digitalWrite(PIN_LED, LOW); return;
        case PLAYING:     digitalWrite(PIN_LED, HIGH); return;
        case RECORDING:   interval = 300; break;
        case FATAL_ERROR: interval = 80; break;
        default:          interval = 120; break; // UPLOADING / PROCESSING / SYNTHESIZING
    }

    if (now - lastToggle >= interval) {
        ledOn = !ledOn;
        digitalWrite(PIN_LED, ledOn ? HIGH : LOW);
        lastToggle = now;
    }
}

// A brief, blocking rapid-blink burst (about 1 second total) used to signal a
// recoverable failure -- a Gemini API call that failed, timed out, or got
// rate-limited. After this returns, the caller drops back to IDLE so the next
// button press just tries again; this is intentionally not treated as fatal.
static void flashError() {
    for (int i = 0; i < 6; i++) {
        digitalWrite(PIN_LED, HIGH);
        delay(80);
        digitalWrite(PIN_LED, LOW);
        delay(80);
    }
}

// Blocks until WiFi connects (or reconnects after a drop). Called once at
// boot, and again from loop() any time WiFi.status() reports disconnected.
static void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(300);
        Serial.print(".");
    }
    Serial.print(" connected, IP ");
    Serial.println(WiFi.localIP());
}

// Runs the upload -> understand -> synthesize -> play sequence for one
// completed recording. `pcmLen` is how many bytes were actually captured
// (recordings shorter than the max buffer are common and expected).
//
// This is called once, synchronously, right after a recording finishes --
// see the comment at the top of this file for why it isn't spread across
// multiple loop() iterations the way RECORDING is.
static void processInteraction(size_t pcmLen) {
    state = UPLOADING;
    updateLed();
    // Wrap the raw PCM (already sitting in pcmBuf, which points into wavBuf --
    // see setup()) with a WAV header describing its format.
    size_t wavLen = wavWrap(pcmBuf, pcmLen, wavBuf, MAX_WAV_BYTES);
    if (wavLen == 0) {
        Serial.println("WAV wrap failed (buffer too small)");
        flashError();
        state = IDLE;
        return;
    }

    state = PROCESSING;
    updateLed();
    String reply;
    if (!understandSpeech(wavBuf, wavLen, reply)) {
        Serial.println("understandSpeech failed");
        flashError();
        state = IDLE;
        return;
    }
    Serial.print("Gemini reply: ");
    Serial.println(reply);

    state = SYNTHESIZING;
    updateLed();
    size_t ttsLen = 0;
    if (!synthesizeSpeech(reply, ttsBuf, ttsLen, MAX_TTS_PCM_BYTES)) {
        Serial.println("synthesizeSpeech failed");
        flashError();
        state = IDLE;
        return;
    }

    state = PLAYING;
    updateLed();
    audioPlayFromBuffer(ttsBuf, ttsLen); // blocks until playback finishes

    state = IDLE;
}

void setup() {
    Serial.begin(115200);
    delay(300); // give the serial monitor a moment to attach before the first prints

    pinMode(PIN_BUTTON, INPUT_PULLUP);
    pinMode(PIN_LED, OUTPUT);

    // Logged so actual PSRAM headroom can be compared against the buffer
    // sizes allocated just below -- useful during hardware bring-up to
    // confirm the memory budget assumptions actually hold on your board.
    Serial.printf("Free PSRAM at boot: %u bytes\n", ESP.getFreePsram());

    audioInit();

    // MALLOC_CAP_SPIRAM forces these allocations into PSRAM specifically,
    // rather than the much smaller internal SRAM -- these buffers (up to
    // ~470KB + ~300KB) would never fit in internal SRAM alongside WiFi/TLS,
    // which is the whole reason this project requires a PSRAM-equipped board.
    wavBuf = (uint8_t*)heap_caps_malloc(MAX_WAV_BYTES, MALLOC_CAP_SPIRAM);
    ttsBuf = (uint8_t*)heap_caps_malloc(MAX_TTS_PCM_BYTES, MALLOC_CAP_SPIRAM);
    if (!wavBuf || !ttsBuf) {
        // Nothing useful can happen without these buffers -- this is the one
        // truly unrecoverable failure mode, unlike API/network errors below.
        Serial.println("Failed to allocate PSRAM audio buffers -- halting");
        state = FATAL_ERROR;
        return;
    }
    // The recording is captured directly into the WAV buffer's payload
    // region (i.e. right after where the 44-byte header will later go),
    // so wavWrap() doesn't need to copy the PCM data anywhere -- it just
    // fills in the header in front of it.
    pcmBuf = wavBuf + WAV_HEADER_SIZE;

    Serial.printf("Free PSRAM after buffer alloc: %u bytes\n", ESP.getFreePsram());

#if !LOOPBACK_TEST_MODE
    // Loopback mode intentionally skips WiFi entirely -- it's meant to
    // validate audio hardware in isolation, with no network involved at all.
    connectWiFi();
#endif

#if WIFI_TEXT_TEST_MODE
    // One-shot sanity check: fire a trivial text query at boot and print
    // whatever comes back, to confirm WiFi/TLS/API key all work before
    // testing the full audio pipeline.
    String reply;
    if (textOnlyQuery("Say hello in five words or fewer.", reply)) {
        Serial.print("Gemini replied: ");
        Serial.println(reply);
    } else {
        Serial.println("WIFI_TEXT_TEST_MODE query failed");
    }
#endif

    Serial.println("Ready. Press and hold the button to talk.");
}

void loop() {
    updateLed();

    if (state == FATAL_ERROR) {
        // PSRAM allocation failed in setup() -- nothing to do but sit here
        // blinking; a physical reset is required to try again.
        return;
    }

#if !LOOPBACK_TEST_MODE
    // WiFi.status() is cheap to check every loop() pass; reconnecting here
    // means a dropped WiFi connection self-heals without a manual reset.
    if (WiFi.status() != WL_CONNECTED) {
        connectWiFi();
    }
#endif

    if (!readButtonPressed()) {
        return; // nothing to do this pass -- stay idle
    }

    // --- Button just pressed: record until it's released ---
    state = RECORDING;
    updateLed();
    Serial.println("Recording...");

    size_t recorded = 0;
    const size_t chunkBytes = 512; // ~16ms of audio per chunk at 16kHz/16-bit -- small enough for prompt release detection
    while (readButtonPressed() && recorded + chunkBytes <= MAX_PCM_BYTES) {
        size_t got = audioReadChunk(pcmBuf + recorded, chunkBytes);
        if (got == 0) break; // I2S read error -- stop early rather than spin forever
        recorded += got;
    }
    Serial.printf("Recorded %u bytes (%.1fs)\n", (unsigned)recorded,
                  recorded / (float)(SAMPLE_RATE_CAPTURE * sizeof(int16_t)));

    if (recorded == 0) {
        // Button was tapped so briefly nothing was actually captured.
        state = IDLE;
        return;
    }

#if LOOPBACK_TEST_MODE
    // Play back exactly what was recorded, no network involved -- this is
    // Build Phase 1: confirming the mic and amp work before anything else.
    state = PLAYING;
    updateLed();
    audioPlayFromBuffer(pcmBuf, recorded);
    state = IDLE;
#else
    // Normal operation: run the full record -> understand -> speak -> play
    // pipeline for what was just captured.
    processInteraction(recorded);
#endif
}
