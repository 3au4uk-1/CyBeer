#pragma once

#include <stdint.h>

void cybeer_display_init(void);
void cybeer_display_show_us(int64_t us);
void cybeer_display_show_zeros(void);
/** Turn TM1637 display off (deep sleep). */
void cybeer_display_blank(void);
