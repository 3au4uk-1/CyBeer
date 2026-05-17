#include <stdint.h>

#include "cybeer_switch.h"
#include "unity.h"

TEST_CASE("test_stable_on_requires_hold", "[switch]")
{
    cybeer_switch_test_reset();
    cybeer_switch_init();
    cybeer_switch_test_set_raw(true);

    cybeer_switch_state_t st;
    const int64_t early_us[] = {0, 50000, 100000, 150000, 200000};
    for (unsigned i = 0; i < sizeof(early_us) / sizeof(early_us[0]); i++) {
        cybeer_switch_poll(early_us[i], &st);
        TEST_ASSERT_FALSE_MESSAGE(st.pressed_stable, "stable before required hold time");
    }

    cybeer_switch_poll(250000, &st);
    TEST_ASSERT_TRUE(st.pressed_stable);
    TEST_ASSERT_TRUE(st.pressed);
}
