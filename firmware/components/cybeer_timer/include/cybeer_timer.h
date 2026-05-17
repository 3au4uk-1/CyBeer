#pragma once

#include <stdbool.h>
#include <stdint.h>

void cybeer_timer_start(int64_t now_us);
void cybeer_timer_stop(int64_t now_us);
int64_t cybeer_timer_elapsed_us(int64_t now_us);
bool cybeer_timer_is_running(void);
