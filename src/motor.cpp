#include "motor.h"
#include "pins.h"
#include "volume_core.h"

// PWM frequency for motor speed control -- well above audible range and well
// within what a small H-bridge/DC motor handles cleanly. Not derived from
// anything else, just a common, safe starting value.
static const int PWM_FREQ_HZ = 5000;
// 8-bit duty cycle (0-255) matches computeVolumeLevel()'s output range
// exactly, so no rescaling is needed between the two.
static const int PWM_RESOLUTION_BITS = 8;

// This project's currently-pinned Arduino-ESP32 core version predates the
// IDF 5.1 LEDC API migration (confirmed by audio.cpp's legacy driver/i2s.h
// calls still compiling against it -- both APIs were retired together) --
// so this uses the channel-based ledcSetup()/ledcAttachPin() API rather than
// the newer pin-based ledcAttach(). If the framework is ever upgraded past
// that migration, this and audio.cpp's I2S calls need to move to their
// respective new APIs together.
static const int MOTOR_A_PWM_CHANNEL = 0;
static const int MOTOR_B_PWM_CHANNEL = 1;

void motorInit() {
    // IN2 on each motor is just held LOW (see pins.h) -- forward-only speed
    // control, no direction/reverse logic needed for a volume-reactive
    // effect. IN1 is the PWM'd pin.
    pinMode(PIN_MOTOR_A_IN2, OUTPUT);
    pinMode(PIN_MOTOR_B_IN2, OUTPUT);
    digitalWrite(PIN_MOTOR_A_IN2, LOW);
    digitalWrite(PIN_MOTOR_B_IN2, LOW);

    ledcSetup(MOTOR_A_PWM_CHANNEL, PWM_FREQ_HZ, PWM_RESOLUTION_BITS);
    ledcAttachPin(PIN_MOTOR_A_IN1, MOTOR_A_PWM_CHANNEL);
    ledcSetup(MOTOR_B_PWM_CHANNEL, PWM_FREQ_HZ, PWM_RESOLUTION_BITS);
    ledcAttachPin(PIN_MOTOR_B_IN1, MOTOR_B_PWM_CHANNEL);

    motorStop();
}

void motorUpdateFromPcm(const uint8_t* pcm, size_t len) {
    size_t sampleCount = len / sizeof(int16_t);
    uint8_t level = computeVolumeLevel((const int16_t*)pcm, sampleCount);
    ledcWrite(MOTOR_A_PWM_CHANNEL, level);
    ledcWrite(MOTOR_B_PWM_CHANNEL, level);
}

void motorStop() {
    ledcWrite(MOTOR_A_PWM_CHANNEL, 0);
    ledcWrite(MOTOR_B_PWM_CHANNEL, 0);
}
