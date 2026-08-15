// ============================================================================
// main.cpp — top-level state machine
// ============================================================================
//
// This is the orchestration layer that ties together audio.h (mic/speaker),
// gemini.h (the two API calls), bluetooth.h (Bluetooth speaker mode),
// motor.h (volume-reactive motors), and pins.h (hardware wiring) into the
// device's actual behavior:
//
//   IDLE (Bluetooth speaker: phone streams music, motors react to it)
//     -> (button pressed) -> RECORDING (stop Bluetooth, start connecting WiFi
//        in the background while recording -- see loop())
//     -> (button released) -> UPLOADING -> PROCESSING -> SYNTHESIZING
//        (the existing Gemini push-to-talk flow, unchanged; motors now also
//        react to the TTS reply's volume)
//     -> tear WiFi back down, resume Bluetooth speaker -> back to IDLE
//
// The reason for that radio dance: the ESP32 has one radio shared between
// WiFi and Bluetooth Classic, and the two don't reliably run at once (see
// enterVoiceMode()/exitVoiceMode() below) -- so Bluetooth is fully stopped
// before every voice interaction and restarted after, rather than the two
// ever being "on" at the same time.
//
// Because the Gemini side of this makes blocking, sequential HTTPS calls (no
// persistent connection, no background tasks), the UPLOADING/PROCESSING/
// SYNTHESIZING states don't each get their own pass through loop() the way
// IDLE and RECORDING do. Instead, processInteraction() runs them back-to-back
// in one go, only updating `state` (and the LED) as a way to report progress
// -- the actual waiting happens inside the blocking calls to
// understandSpeech() and synthesizeSpeech(). synthesizeSpeech() itself
// streams the TTS reply and plays each decoded chunk as it arrives (see
// playTtsChunk() below) rather than decoding the whole reply first -- this
// board has no PSRAM to hold a whole reply's worth of audio in at once.

#include <Arduino.h>
#include <WiFi.h>
#include "pins.h"
#include "config.h"
#include "audio.h"
#include "gemini.h"
#include "bluetooth.h"
#include "motor.h"

enum State { IDLE, RECORDING, UPLOADING, PROCESSING, SYNTHESIZING, PLAYING, FATAL_ERROR };

static State state = IDLE;

// Worst-case size of a recording: MAX_RECORDING_SECONDS worth of 16-bit
// samples at SAMPLE_RATE_CAPTURE. This bounds how big wavBuf below needs to
// be, and is the hard ceiling the RECORDING loop enforces so a stuck/taped-
// down button can't grow memory use without limit. See MAX_RECORDING_SECONDS
// in pins.h for why this is sized the way it is on a PSRAM-less board.
static const size_t MAX_PCM_BYTES = (size_t)MAX_RECORDING_SECONDS * SAMPLE_RATE_CAPTURE * sizeof(int16_t);
static const size_t MAX_WAV_BYTES = MAX_PCM_BYTES + WAV_HEADER_SIZE;

// The one large buffer this firmware still holds in full: the raw recording,
// captured before understandSpeech() can be called at all (its final length
// isn't known until the button is released, and the WAV header needs an
// exact size). Allocated once in setup() and reused for every interaction,
// in plain internal heap -- there's no PSRAM on this board to put it in.
// Both the base64-encoded upload and the decoded TTS reply are streamed
// instead of buffered (see gemini.cpp), so neither needs a buffer here.
static uint8_t* pcmBuf = nullptr; // raw mic recording -- actually just points partway into wavBuf, see setup()
static uint8_t* wavBuf = nullptr; // WAV header + pcmBuf's data, contiguous in one allocation

// UI cue tones played around the Bluetooth/WiFi radio switch (see
// enterVoiceMode()/exitVoiceMode() below) -- the switch itself isn't
// instant (a full Bluetooth teardown costs real time, see bluetooth.cpp),
// so these turn an otherwise-silent, potentially multi-second gap into a
// deliberate-feeling "listening now" / "back to being a speaker" moment.
// Different pitches (rising vs falling) make the two cues distinguishable
// by ear without looking at the LED.
static const uint32_t TONE_ENTER_VOICE_HZ = 1200;
static const uint32_t TONE_EXIT_VOICE_HZ = 700;
static const uint32_t TONE_DURATION_MS = 150;

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

// Passed to synthesizeSpeech() as its per-chunk callback: plays each decoded
// piece of the TTS reply as soon as it arrives instead of waiting for the
// whole reply to be decoded first. Also nudges the LED so the busy-blink
// actually animates during this phase now, instead of only snapping to a
// fixed level the way a single blocking call would (see the file-level
// comment above and updateLed()'s own comment for why that used to be true).
// Gemini's TTS audio is mono, but I2S1 is always configured for stereo (2
// amps -- see audio.cpp), so this upmixes rather than writing raw bytes
// directly. Motors react to this audio's volume the same way they react to
// Bluetooth music (see bluetooth.cpp's onAudioData()).
static void playTtsChunk(const uint8_t* pcm, size_t len) {
    updateLed();
    audioPlayMonoAsStereo(pcm, len);
    motorUpdateFromPcm(pcm, len);
}

// Finishes the radio switch loop() started at button-press (see loop()):
// blocks until WiFi is connected (usually near-instant here, since the
// connection was kicked off back when recording started), reconfigures I2S1
// for a voice reply's sample rate, and stops the motors -- no audio plays
// through the speaker during UPLOADING/PROCESSING, so nothing should be
// driving them until synthesizeSpeech() starts calling playTtsChunk().
static void enterVoiceMode() {
    connectWiFi();
    audioSetPlaybackRate(SAMPLE_RATE_PLAYBACK);
    motorStop();
}

// Reverses enterVoiceMode(): tears WiFi down and resumes Bluetooth speaker
// mode. Used both after a normal interaction finishes and to undo the radio
// switch if a button tap turned out to be too brief to record anything (see
// loop()) -- in both cases, WiFi's job here is done and Bluetooth should
// have the radio back.
static void exitVoiceMode() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    audioSetPlaybackRate(SAMPLE_RATE_BLUETOOTH);
    bluetoothStart(); // proactively reconnects to the last-paired phone, see bluetooth.cpp
    audioPlayTone(SAMPLE_RATE_BLUETOOTH, TONE_EXIT_VOICE_HZ, TONE_DURATION_MS);
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

    // SYNTHESIZING and PLAYING are no longer two separate steps here --
    // synthesizeSpeech() calls playTtsChunk() once per decoded chunk as the
    // reply streams in, so decoding and playback happen interleaved. `state`
    // is set to SYNTHESIZING first since that's the more accurate label for
    // most of this call's duration (network wait dominates over the brief
    // per-chunk playback); playTtsChunk() bumps the LED on every call so the
    // busy-blink still animates throughout, unlike the old single blocking
    // synthesizeSpeech() + audioPlayFromBuffer() pair.
    state = SYNTHESIZING;
    updateLed();
    if (!synthesizeSpeech(reply, playTtsChunk)) {
        Serial.println("synthesizeSpeech failed");
        flashError();
        state = IDLE;
        return;
    }

    state = IDLE;
}

void setup() {
    Serial.begin(115200);
    delay(300); // give the serial monitor a moment to attach before the first prints

    pinMode(PIN_BUTTON, INPUT_PULLUP);
    pinMode(PIN_LED, OUTPUT);

    // Logged so actual free-heap headroom can be compared against wavBuf's
    // size just below -- useful during hardware bring-up to confirm
    // MAX_RECORDING_SECONDS (pins.h) is actually a safe fit alongside
    // WiFi/TLS on your specific board, not just a guess.
    Serial.printf("Free heap at boot: %u bytes\n", ESP.getFreeHeap());

    audioInit();
    motorInit(); // configures PWM and leaves both motors at 0 speed

    // Plain internal-heap allocation -- this board (ESP32-WROOM-32D) has no
    // PSRAM, so unlike the original WROVER design there's no MALLOC_CAP_SPIRAM
    // capability to request. wavBuf is the only large buffer left in this
    // firmware (the base64-encoded upload and the decoded TTS reply are both
    // streamed instead -- see gemini.cpp), which is what makes fitting it in
    // internal SRAM feasible at all.
    wavBuf = (uint8_t*)malloc(MAX_WAV_BYTES);
    if (!wavBuf) {
        // Nothing useful can happen without this buffer -- this is the one
        // truly unrecoverable failure mode, unlike API/network errors below.
        Serial.println("Failed to allocate recording buffer -- halting");
        state = FATAL_ERROR;
        return;
    }
    // The recording is captured directly into the WAV buffer's payload
    // region (i.e. right after where the 44-byte header will later go),
    // so wavWrap() doesn't need to copy the PCM data anywhere -- it just
    // fills in the header in front of it.
    pcmBuf = wavBuf + WAV_HEADER_SIZE;

    Serial.printf("Free heap after buffer alloc: %u bytes\n", ESP.getFreeHeap());

#if LOOPBACK_TEST_MODE
    // Loopback mode intentionally skips WiFi and Bluetooth entirely -- it's
    // meant to validate audio hardware in isolation, with no radio involved
    // at all.
#elif WIFI_TEXT_TEST_MODE
    // One-shot sanity check: connect WiFi directly (skipping Bluetooth
    // speaker mode, since this test mode only validates WiFi/TLS/the API key
    // in isolation) and fire a trivial text query, printing whatever comes
    // back.
    connectWiFi();
    String reply;
    if (textOnlyQuery("Say hello in five words or fewer.", reply)) {
        Serial.print("Gemini replied: ");
        Serial.println(reply);
    } else {
        Serial.println("WIFI_TEXT_TEST_MODE query failed");
    }
#else
    // Normal operation: Bluetooth speaker is the default idle behavior --
    // WiFi only comes up on-demand around a voice interaction (see loop()),
    // since this chip's one radio can't reliably run WiFi and Bluetooth
    // Classic at the same time.
    bluetoothStart();
#endif

    Serial.println("Ready. Press and hold the button to talk.");
}

void loop() {
    updateLed();

    if (state == FATAL_ERROR) {
        // Recording buffer allocation failed in setup() -- nothing to do but
        // sit here blinking; a physical reset is required to try again.
        return;
    }

    // No background WiFi-reconnect check here any more -- at idle, WiFi
    // being disconnected is the *expected* state (Bluetooth owns the radio
    // then, see enterVoiceMode()/exitVoiceMode()), not a fault to correct.
    // WiFi is brought up fresh for each voice interaction instead.

    if (!readButtonPressed()) {
        return; // nothing to do this pass -- stay idle (Bluetooth speaker, if connected, keeps playing via its own task)
    }

    // --- Button just pressed: record until it's released ---
    state = RECORDING;
    updateLed();
    Serial.println("Recording...");

#if !LOOPBACK_TEST_MODE
    // Kick off the radio switch now, in parallel with recording below,
    // instead of waiting until the button is released -- by the time
    // recording ends, WiFi has often already finished connecting, hiding
    // most of the switch latency (finished off by enterVoiceMode() below).
    // bluetoothStop()'s full teardown isn't instant (see bluetooth.cpp) --
    // the tone right after it turns that gap into a deliberate-feeling
    // "listening now" cue instead of unexplained silence.
    bluetoothStop();
    audioPlayTone(SAMPLE_RATE_BLUETOOTH, TONE_ENTER_VOICE_HZ, TONE_DURATION_MS);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
#endif

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
        // Button was tapped so briefly nothing was actually captured -- no
        // interaction will run, so undo the radio switch started above.
#if !LOOPBACK_TEST_MODE
        exitVoiceMode();
#endif
        state = IDLE;
        return;
    }

#if LOOPBACK_TEST_MODE
    // Play back exactly what was recorded, no radio involved at all -- this
    // is Build Phase 1: confirming the mic and amp work before anything
    // else. Upmixed to stereo and played at the capture rate (I2S1 is always
    // stereo now -- see audio.cpp -- and defaults to SAMPLE_RATE_BLUETOOTH,
    // which would otherwise play this back at the wrong pitch/speed).
    state = PLAYING;
    updateLed();
    audioSetPlaybackRate(SAMPLE_RATE_CAPTURE);
    audioPlayMonoAsStereo(pcmBuf, recorded);
    audioSetPlaybackRate(SAMPLE_RATE_BLUETOOTH);
    state = IDLE;
#else
    // Normal operation: finish the radio switch, run the full record ->
    // understand -> speak -> play pipeline for what was just captured, then
    // switch back to being a Bluetooth speaker.
    enterVoiceMode();
    processInteraction(recorded);
    exitVoiceMode();
    state = IDLE;
#endif
}
