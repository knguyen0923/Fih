// Pin-map sanity check, included by test_main.cpp. Pure preprocessor values,
// so it runs on the host -- no board needed.
#pragma once
#include <stdio.h>
#include <unity.h>
#include "pins.h"

void test_no_gpio_assigned_twice(void) {
    struct { const char* name; int pin; } pins[] = {
        {"MIC_BCLK", PIN_MIC_BCLK}, {"MIC_WS", PIN_MIC_WS}, {"MIC_DATA", PIN_MIC_DATA},
        {"AMP_BCLK", PIN_AMP_BCLK}, {"AMP_WS", PIN_AMP_WS}, {"AMP_DATA", PIN_AMP_DATA},
        {"MOTOR_A_IN1", PIN_MOTOR_A_IN1}, {"MOTOR_A_IN2", PIN_MOTOR_A_IN2},
        {"MOTOR_B_IN1", PIN_MOTOR_B_IN1}, {"MOTOR_B_IN2", PIN_MOTOR_B_IN2},
        {"BUTTON", PIN_BUTTON}, {"LED", PIN_LED},
    };
    const int n = sizeof(pins) / sizeof(pins[0]);
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            char msg[64];
            snprintf(msg, sizeof(msg), "GPIO %d used by both %s and %s",
                     pins[i].pin, pins[i].name, pins[j].name);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(pins[i].pin, pins[j].pin, msg);
        }
    }
}

void test_pins_are_valid_esp32_gpios(void) {
    int out[] = {PIN_MIC_BCLK, PIN_MIC_WS, PIN_AMP_BCLK, PIN_AMP_WS, PIN_AMP_DATA,
                 PIN_MOTOR_A_IN1, PIN_MOTOR_A_IN2, PIN_MOTOR_B_IN1, PIN_MOTOR_B_IN2, PIN_LED};
    for (int p : out) {
        TEST_ASSERT_TRUE(p >= 0 && p <= 33);                  // output-capable range
        TEST_ASSERT_FALSE(p >= 6 && p <= 11);                 // flash pins
    }
    // Input-only pins (34-39) are fine for mic data and the button only.
    TEST_ASSERT_TRUE(PIN_MIC_DATA >= 0 && PIN_MIC_DATA <= 39);
    TEST_ASSERT_TRUE(PIN_BUTTON >= 0 && PIN_BUTTON <= 39);
}
