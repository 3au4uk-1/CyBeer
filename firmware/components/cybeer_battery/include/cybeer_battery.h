#pragma once

#include "esp_err.h"

typedef void (*cybeer_battery_listener_t)(int percent);

esp_err_t cybeer_battery_init(void);
void cybeer_battery_register_listener(cybeer_battery_listener_t fn);

int cybeer_battery_get_percent(void);

/**
 * @return Nominal Li-ion cell voltage (volts), assuming a 2:1 divider on the ADC pin.
 */
float cybeer_battery_get_voltage(void);
