// ============================================================================
// test_device/test_main.cpp — on-device tests for every hardware component
// ============================================================================
//
// Run with: pio test -e esp32_test      (board attached over USB, fully wired)
//
// Covers what the native suite can't: motor PWM, mic capture, speaker output,
// Bluetooth stop/start cycling, WiFi, and the three Gemini calls. Network
// tests use real API quota. A test that needs a person (mic, speaker) says so
// in its name/comment -- listen/speak when it runs.

#include <Arduino.h>
#include <WiFi.h>
#include <unity.h>
#include <esp_bt.h>
#include "config.h"
#include "pins.h"
#include "audio.h"
#include "motor.h"
#include "bluetooth.h"
#include "gemini.h"
#include "wav.h"

// motor.cpp's LEDC channels (static there; mirrored here for ledcRead()).
static const int MOTOR_A_CH = 0;
static const int MOTOR_B_CH = 1;

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------- memory ---

void test_recording_buffer_fits_in_heap(void) {
    size_t bytes = (size_t)MAX_RECORDING_SECONDS * SAMPLE_RATE_CAPTURE * sizeof(int16_t) + WAV_HEADER_SIZE;
    uint8_t* p = (uint8_t*)malloc(bytes);
    TEST_ASSERT_NOT_NULL_MESSAGE(p, "wavBuf allocation failed -- lower MAX_RECORDING_SECONDS");
    free(p);
}

// ----------------------------------------------------------- button / LED ---

void test_button_idle_reads_not_pressed(void) {
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    delay(20);
    TEST_ASSERT_EQUAL_MESSAGE(HIGH, digitalRead(PIN_BUTTON),
        "button reads LOW while untouched: shorted, wired active-high, or pin shared with another output");
}

void test_led_toggles(void) {
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH);
    TEST_ASSERT_EQUAL(HIGH, digitalRead(PIN_LED));
    digitalWrite(PIN_LED, LOW);
    TEST_ASSERT_EQUAL(LOW, digitalRead(PIN_LED));
}

// ----------------------------------------------------------------- motors ---

static void fillPcm(int16_t* buf, size_t n, int16_t value) {
    for (size_t i = 0; i < n; i++) buf[i] = (i & 1) ? value : -value;
}

void test_motor_stops_at_init(void) {
    motorInit();
    TEST_ASSERT_EQUAL_UINT32(0, ledcRead(MOTOR_A_CH));
    TEST_ASSERT_EQUAL_UINT32(0, ledcRead(MOTOR_B_CH));
}

void test_motor_silence_gives_zero_duty(void) {
    int16_t buf[256] = {0};
    motorUpdateFromPcm((uint8_t*)buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(0, ledcRead(MOTOR_A_CH));
    TEST_ASSERT_EQUAL_UINT32(0, ledcRead(MOTOR_B_CH));
}

void test_motor_loud_audio_raises_both_motors(void) {
    int16_t buf[256];
    fillPcm(buf, 256, 20000);
    motorUpdateFromPcm((uint8_t*)buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_UINT32(0, ledcRead(MOTOR_A_CH));
    TEST_ASSERT_EQUAL_UINT32(ledcRead(MOTOR_A_CH), ledcRead(MOTOR_B_CH));
}

void test_motor_louder_is_faster(void) {
    int16_t quiet[256], loud[256];
    fillPcm(quiet, 256, 2000);
    fillPcm(loud, 256, 20000);
    motorUpdateFromPcm((uint8_t*)quiet, sizeof(quiet));
    uint32_t q = ledcRead(MOTOR_A_CH);
    motorUpdateFromPcm((uint8_t*)loud, sizeof(loud));
    TEST_ASSERT_GREATER_THAN_UINT32(q, ledcRead(MOTOR_A_CH));
}

void test_motor_stop_zeroes_both(void) {
    int16_t buf[256];
    fillPcm(buf, 256, 20000);
    motorUpdateFromPcm((uint8_t*)buf, sizeof(buf));
    motorStop();
    TEST_ASSERT_EQUAL_UINT32(0, ledcRead(MOTOR_A_CH));
    TEST_ASSERT_EQUAL_UINT32(0, ledcRead(MOTOR_B_CH));
}

void test_motor_in2_pins_held_low(void) {
    TEST_ASSERT_EQUAL(LOW, digitalRead(PIN_MOTOR_A_IN2));
    TEST_ASSERT_EQUAL(LOW, digitalRead(PIN_MOTOR_B_IN2));
}

// ------------------------------------------------------------------ audio ---

void test_audio_init_and_mic_returns_data(void) {
    audioInit();
    uint8_t buf[1024];
    size_t got = 0;
    unsigned long start = millis();
    while (got == 0 && millis() - start < 2000) got = audioReadChunk(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, got, "mic produced no samples within 2s -- check mic wiring/pins");
}

void test_mic_not_stuck_at_constant_value(void) {
    // Speak or tap near the mic during this test. A dead/floating data line
    // gives one constant value forever.
    uint8_t buf[2048];
    int16_t lo = INT16_MAX, hi = INT16_MIN;
    unsigned long start = millis();
    while (millis() - start < 1500) {
        size_t got = audioReadChunk(buf, sizeof(buf));
        const int16_t* s = (const int16_t*)buf;
        for (size_t i = 0; i < got / 2; i++) { if (s[i] < lo) lo = s[i]; if (s[i] > hi) hi = s[i]; }
    }
    TEST_ASSERT_NOT_EQUAL_MESSAGE(lo, hi, "mic samples never changed -- data pin not connected?");
}

void test_play_tone_returns_in_time(void) {
    // LISTEN: you should hear a ~300ms beep from both speakers.
    unsigned long t = millis();
    audioPlayTone(SAMPLE_RATE_PLAYBACK, 880, 300);  // sets the I2S rate itself
    unsigned long took = millis() - t;
    TEST_ASSERT_UINT32_WITHIN(150, 300, took);
}

void test_play_mono_as_stereo_returns(void) {
    static int16_t mono[2400];  // 100ms @ 24kHz
    for (int i = 0; i < 2400; i++) mono[i] = (int16_t)(8000 * sinf(i * 0.2f));
    audioSetPlaybackRate(SAMPLE_RATE_PLAYBACK);
    audioPlayMonoAsStereo((uint8_t*)mono, sizeof(mono));
    TEST_PASS();
}

void test_wav_wrap_of_real_recording_size(void) {
    size_t pcmLen = (size_t)MAX_RECORDING_SECONDS * SAMPLE_RATE_CAPTURE * sizeof(int16_t);
    uint8_t* buf = (uint8_t*)malloc(pcmLen + WAV_HEADER_SIZE);
    TEST_ASSERT_NOT_NULL(buf);
    memset(buf + WAV_HEADER_SIZE, 0, pcmLen);
    // PCM already sits right after the header slot, as in main.cpp's in-place use.
    TEST_ASSERT_EQUAL(pcmLen + WAV_HEADER_SIZE, wavWrap(buf + WAV_HEADER_SIZE, pcmLen, buf, pcmLen + WAV_HEADER_SIZE));
    TEST_ASSERT_EQUAL_MEMORY("RIFF", buf, 4);
    TEST_ASSERT_EQUAL_MEMORY("WAVE", buf + 8, 4);
    free(buf);
}

// -------------------------------------------------------------- bluetooth ---

void test_bluetooth_stop_frees_controller_and_restart_works(void) {
    // Regression: end(true) permanently disabled restart after the first
    // voice interaction. Cycle twice and confirm the controller comes back.
    bluetoothStart();
    delay(500);
    TEST_ASSERT_EQUAL(ESP_BT_CONTROLLER_STATUS_ENABLED, esp_bt_controller_get_status());

    bluetoothStop();
    TEST_ASSERT_EQUAL_MESSAGE(ESP_BT_CONTROLLER_STATUS_IDLE, esp_bt_controller_get_status(),
        "controller still up after bluetoothStop() -- WiFi would contend with it");

    bluetoothStart();
    delay(500);
    TEST_ASSERT_EQUAL_MESSAGE(ESP_BT_CONTROLLER_STATUS_ENABLED, esp_bt_controller_get_status(),
        "Bluetooth did not restart after stop");
}

void test_bluetooth_repeated_cycles_do_not_leak_heap(void) {
    uint32_t before = ESP.getFreeHeap();
    for (int i = 0; i < 3; i++) { bluetoothStop(); bluetoothStart(); delay(300); }
    uint32_t after = ESP.getFreeHeap();
    TEST_ASSERT_GREATER_THAN_MESSAGE(before - 8192, after, "heap shrank >8KB over 3 stop/start cycles");
}

void test_bluetooth_mute_unmute_do_not_crash(void) {
    bluetoothMute();
    bluetoothUnmute();
    bluetoothMute();
    bluetoothUnmute();
    TEST_PASS();
}

// ------------------------------------------------------------ wifi/gemini ---

static bool wifiUp() {
    bluetoothStop();  // one radio: BT must be down before WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(100);
    return WiFi.status() == WL_CONNECTED;
}

void test_wifi_connects(void) {
    TEST_ASSERT_TRUE_MESSAGE(wifiUp(), "WiFi did not connect in 15s -- check SSID/password in config.h");
}

void test_gemini_text_query(void) {
    String reply;
    TEST_ASSERT_TRUE(textOnlyQuery("Reply with exactly one word: OK", reply));
    TEST_ASSERT_GREATER_THAN(0, reply.length());
    Serial.printf("text reply: %s\n", reply.c_str());
}

static size_t ttsBytes = 0;
static bool ttsOddChunk = false;
static void countChunk(const uint8_t*, size_t len) {
    ttsBytes += len;
    if (len & 1) ttsOddChunk = true;  // 16-bit PCM chunks should be whole samples
}

void test_gemini_tts_streams_pcm(void) {
    ttsBytes = 0;
    ttsOddChunk = false;
    TEST_ASSERT_TRUE(synthesizeSpeech("Hello from the test suite.", countChunk));
    // ~1s of 24kHz 16-bit mono is 48000 bytes; anything tiny means truncation.
    TEST_ASSERT_GREATER_THAN_MESSAGE(20000, ttsBytes, "TTS reply suspiciously short -- truncated stream?");
    TEST_ASSERT_FALSE_MESSAGE(ttsOddChunk, "got a chunk with an odd byte count -- stream/dechunk bug");
    Serial.printf("tts bytes: %u\n", (unsigned)ttsBytes);
}

void test_gemini_understand_speech_accepts_wav(void) {
    // 1s of 440Hz tone, not speech; just checks upload + response parsing.
    const size_t samples = SAMPLE_RATE_CAPTURE;
    size_t pcmLen = samples * sizeof(int16_t);
    uint8_t* wav = (uint8_t*)malloc(pcmLen + WAV_HEADER_SIZE);
    TEST_ASSERT_NOT_NULL(wav);
    int16_t* pcm = (int16_t*)(wav + WAV_HEADER_SIZE);
    for (size_t i = 0; i < samples; i++) pcm[i] = (int16_t)(8000 * sinf(2 * PI * 440 * i / SAMPLE_RATE_CAPTURE));
    TEST_ASSERT_EQUAL(pcmLen + WAV_HEADER_SIZE, wavWrap((uint8_t*)pcm, pcmLen, wav, pcmLen + WAV_HEADER_SIZE));
    String reply;
    bool ok = understandSpeech(wav, pcmLen + WAV_HEADER_SIZE, reply);
    free(wav);
    TEST_ASSERT_TRUE(ok);
    Serial.printf("understand reply: %s\n", reply.c_str());
}

void test_wifi_teardown_then_bluetooth_resumes(void) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    bluetoothStart();
    delay(500);
    TEST_ASSERT_EQUAL(ESP_BT_CONTROLLER_STATUS_ENABLED, esp_bt_controller_get_status());
}

void setup() {
    Serial.begin(115200);
    delay(2000);  // let the test runner attach
    UNITY_BEGIN();
    RUN_TEST(test_recording_buffer_fits_in_heap);
    RUN_TEST(test_button_idle_reads_not_pressed);
    RUN_TEST(test_led_toggles);
    RUN_TEST(test_motor_stops_at_init);
    RUN_TEST(test_motor_silence_gives_zero_duty);
    RUN_TEST(test_motor_loud_audio_raises_both_motors);
    RUN_TEST(test_motor_louder_is_faster);
    RUN_TEST(test_motor_stop_zeroes_both);
    RUN_TEST(test_motor_in2_pins_held_low);
    RUN_TEST(test_audio_init_and_mic_returns_data);
    RUN_TEST(test_mic_not_stuck_at_constant_value);
    RUN_TEST(test_play_tone_returns_in_time);
    RUN_TEST(test_play_mono_as_stereo_returns);
    RUN_TEST(test_wav_wrap_of_real_recording_size);
    RUN_TEST(test_bluetooth_stop_frees_controller_and_restart_works);
    RUN_TEST(test_bluetooth_repeated_cycles_do_not_leak_heap);
    RUN_TEST(test_bluetooth_mute_unmute_do_not_crash);
    RUN_TEST(test_wifi_connects);
    RUN_TEST(test_gemini_text_query);
    RUN_TEST(test_gemini_tts_streams_pcm);
    RUN_TEST(test_gemini_understand_speech_accepts_wav);
    RUN_TEST(test_wifi_teardown_then_bluetooth_resumes);
    UNITY_END();
}

void loop() {}
