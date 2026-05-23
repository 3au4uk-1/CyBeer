#include "cybeer_fsm.h"

#include "cybeer_timer.h"

static cybeer_state_t s_state;
static cybeer_fsm_callbacks_t s_callbacks;
static int64_t s_finished_duration_us;

void cybeer_fsm_init(const cybeer_fsm_callbacks_t *callbacks)
{
    if (callbacks != NULL) {
        s_callbacks = *callbacks;
    } else {
        s_callbacks.on_finished = NULL;
        s_callbacks.user_ctx = NULL;
    }
}

void cybeer_fsm_reset_to_prep(int64_t now_us)
{
    cybeer_timer_stop(now_us);
    s_state = CYBEER_STATE_PREP;
    s_finished_duration_us = 0;
}

void cybeer_fsm_reset_to_ready(int64_t now_us)
{
    cybeer_timer_stop(now_us);
    s_state = CYBEER_STATE_READY;
    s_finished_duration_us = 0;
}

void cybeer_fsm_on_switch_stable(bool pressed, int64_t now_us)
{
    switch (s_state) {
    case CYBEER_STATE_PREP:
        if (!pressed) {
            s_state = CYBEER_STATE_RUNNING;
            cybeer_timer_start(now_us);
        }
        break;
    case CYBEER_STATE_RUNNING:
        if (pressed) {
            cybeer_timer_stop(now_us);
            s_finished_duration_us = cybeer_timer_elapsed_us(now_us);
            s_state = CYBEER_STATE_FINISHED;
            if (s_callbacks.on_finished != NULL) {
                s_callbacks.on_finished(s_finished_duration_us, s_callbacks.user_ctx);
            }
        }
        break;
    case CYBEER_STATE_FINISHED:
        if (!pressed) {
            s_state = CYBEER_STATE_READY;
        }
        break;
    case CYBEER_STATE_READY:
        if (pressed) {
            s_state = CYBEER_STATE_PREP;
        }
        break;
    default:
        break;
    }
}

cybeer_fsm_snapshot_t cybeer_fsm_snapshot(void)
{
    cybeer_fsm_snapshot_t snap;
    snap.state = s_state;
    snap.finished_duration_us = s_finished_duration_us;
    return snap;
}
