#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Reset idle sleep timer (call on user-visible activity). */
void cybeer_power_note_activity(void);

/**
 * After deep sleep GPIO wake: require triple tap on CYBEER_GPIO_SWITCH.
 * On failure enters deep sleep again (does not return).
 * On cold boot / reset returns true immediately.
 */
bool cybeer_power_confirm_wake_or_sleep(void);

/**
 * Enter deep sleep if idle timeout elapsed and device may sleep.
 * Does not return when sleeping.
 */
void cybeer_power_maybe_sleep(bool ota_active, bool timer_running);
