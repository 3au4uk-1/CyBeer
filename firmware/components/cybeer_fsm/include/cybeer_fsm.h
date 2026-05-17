#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CYBEER_STATE_PREP = 0,
    CYBEER_STATE_RUNNING,
    CYBEER_STATE_FINISHED,
    CYBEER_STATE_READY,
} cybeer_state_t;

typedef struct {
    cybeer_state_t state;
    /** Duration of the last completed run; updated on enter FINISHED */
    int64_t finished_duration_us;
} cybeer_fsm_snapshot_t;

typedef void (*cybeer_fsm_on_finished_t)(int64_t duration_us, void *user_ctx);

typedef struct {
    cybeer_fsm_on_finished_t on_finished;
    void *user_ctx;
} cybeer_fsm_callbacks_t;

void cybeer_fsm_init(const cybeer_fsm_callbacks_t *callbacks);
void cybeer_fsm_reset_to_prep(int64_t now_us);
void cybeer_fsm_on_switch_stable(bool pressed, int64_t now_us);
cybeer_fsm_snapshot_t cybeer_fsm_snapshot(void);
