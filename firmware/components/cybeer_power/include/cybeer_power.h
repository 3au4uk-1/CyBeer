#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Reset idle sleep timer (call on user-visible activity). */
void cybeer_power_note_activity(void);

/**
 * Idle light sleep after CYBEER_IDLE_SLEEP_MS without switch/FSM activity.
 * Wake on GPIO9 (short taps); three taps within the window unlock normal operation.
 * ESP32-C3 deep-sleep GPIO wake is limited to GPIO0–5, so light sleep is used for GPIO9.
 */
bool cybeer_power_confirm_wake_or_sleep(void);

/**
 * Enter light sleep if idle timeout elapsed. Blocks until triple-tap wake unlock.
 * Does not return while sleeping.
 */
void cybeer_power_maybe_sleep(bool ota_active, bool timer_running);
