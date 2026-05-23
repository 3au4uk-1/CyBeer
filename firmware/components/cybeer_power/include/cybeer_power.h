#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Reset idle timer (meaningful user activity). Also exits idle-latch if active. */
void cybeer_power_note_activity(void);

/** True after idle timeout: display/LED off until triple-tap wake. */
bool cybeer_power_is_idle(void);

/** Boot hook: clear stale GPIO wake config from prior firmware. */
bool cybeer_power_confirm_wake_or_sleep(void);

/** Poll triple-tap wake while idle-latched (call every display loop). */
void cybeer_power_poll(void);

/** Enter idle-latch when timeout elapsed; poll wake taps when already idle. */
void cybeer_power_maybe_sleep(bool ota_active, bool timer_running);

/** Toggle eco mode (display/LED off, Wi-Fi stays up). Returns new state. */
bool cybeer_power_toggle_eco(void);

/** True when admin eco mode is active. */
bool cybeer_power_is_eco(void);

/** Enter light sleep until triple-tap wake (blocks; call from dedicated task). */
void cybeer_power_trigger_sleep(void);
