#include <stdint.h>

#include "cybeer_fsm.h"
#include "cybeer_timer.h"
#include "unity.h"

static int s_finished_calls;
static int64_t s_last_finished_us;

static void test_on_finished(int64_t duration_us, void *user_ctx)
{
    (void)user_ctx;
    s_finished_calls++;
    s_last_finished_us = duration_us;
}

TEST_CASE("test_prep_running_finished_ready_prep", "[fsm]")
{
    const cybeer_fsm_callbacks_t cb = {
        .on_finished = test_on_finished,
        .user_ctx = NULL,
    };

    cybeer_fsm_init(&cb);
    s_finished_calls = 0;
    s_last_finished_us = -1;

    cybeer_fsm_reset_to_prep(0);
    TEST_ASSERT_EQUAL_INT(CYBEER_STATE_PREP, cybeer_fsm_snapshot().state);

    cybeer_fsm_on_switch_stable(false, 100000);
    TEST_ASSERT_EQUAL_INT(CYBEER_STATE_RUNNING, cybeer_fsm_snapshot().state);
    TEST_ASSERT_TRUE(cybeer_timer_is_running());

    cybeer_fsm_on_switch_stable(true, 200000);
    TEST_ASSERT_EQUAL_INT(CYBEER_STATE_FINISHED, cybeer_fsm_snapshot().state);
    TEST_ASSERT_FALSE(cybeer_timer_is_running());
    TEST_ASSERT_EQUAL_INT(1, s_finished_calls);
    TEST_ASSERT_TRUE(s_last_finished_us >= 0);
    {
        const cybeer_fsm_snapshot_t snap = cybeer_fsm_snapshot();
        TEST_ASSERT_TRUE(snap.finished_duration_us == s_last_finished_us);
    }

    cybeer_fsm_on_switch_stable(false, 300000);
    TEST_ASSERT_EQUAL_INT(CYBEER_STATE_READY, cybeer_fsm_snapshot().state);

    cybeer_fsm_on_switch_stable(true, 400000);
    TEST_ASSERT_EQUAL_INT(CYBEER_STATE_PREP, cybeer_fsm_snapshot().state);
}
