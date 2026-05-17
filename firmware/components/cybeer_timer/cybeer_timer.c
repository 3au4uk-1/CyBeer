#include "cybeer_timer.h"

#include "esp_timer.h"

static int64_t s_t0_us;
static int64_t s_t1_us;
static bool s_running;

void cybeer_timer_start(int64_t now_us)
{
    (void)now_us;
    s_t0_us = esp_timer_get_time();
    s_running = true;
}

void cybeer_timer_stop(int64_t now_us)
{
    (void)now_us;
    if (s_running) {
        s_t1_us = esp_timer_get_time();
        s_running = false;
    }
}

int64_t cybeer_timer_elapsed_us(int64_t now_us)
{
    (void)now_us;
    if (s_running) {
        return esp_timer_get_time() - s_t0_us;
    }
    return s_t1_us - s_t0_us;
}

bool cybeer_timer_is_running(void)
{
    return s_running;
}
