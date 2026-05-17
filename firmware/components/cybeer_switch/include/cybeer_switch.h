#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool pressed;        /* raw debounced level: true = bottle ON platform (LOW) */
    bool pressed_stable; /* true after stable ON or stable OFF hold time */
} cybeer_switch_state_t;

void cybeer_switch_init(void);
void cybeer_switch_poll(int64_t now_us, cybeer_switch_state_t *out);

#if CONFIG_CYBEER_SWITCH_TEST
void cybeer_switch_test_set_raw(bool bottle_on_platform);
void cybeer_switch_test_reset(void);
#endif
