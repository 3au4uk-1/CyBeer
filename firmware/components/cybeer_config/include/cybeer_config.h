#pragma once

#define CYBEER_GPIO_SWITCH      9
#define CYBEER_GPIO_TM1637_CLK  6
#define CYBEER_GPIO_TM1637_DIO  7
/** Set to 1 if the TM1637 module is mounted upside down (180°). */
#define CYBEER_DISPLAY_FLIP     1
#define CYBEER_GPIO_LED_DATA    8
#define CYBEER_GPIO_BATTERY_ADC 0

#define CYBEER_DEBOUNCE_MS          40
#define CYBEER_STABLE_ON_MS         200
#define CYBEER_STABLE_OFF_MS        150

/** Deep sleep after this idle period (no switch / FSM activity). */
#define CYBEER_IDLE_SLEEP_MS        (5 * 60 * 1000)
/** GPIO wake: number of short taps required to stay awake. */
#define CYBEER_WAKE_CLICK_COUNT     3
#define CYBEER_WAKE_CLICK_WINDOW_MS 3000
#define CYBEER_WAKE_CLICK_MIN_MS    40
#define CYBEER_WAKE_CLICK_MAX_MS    600
#define CYBEER_WAKE_CLICK_GAP_MS    900

#define CYBEER_LED_COUNT_MAX        64
#define CYBEER_LED_COUNT_DEFAULT    20
#define CYBEER_LED_BRIGHTNESS_DEFAULT 64

#define CYBEER_ADMIN_PIN_MAX_LEN    16
#define CYBEER_MDNS_HOSTNAME        "cybeer"

#define CYBEER_OTA_MANIFEST_URL "https://raw.githubusercontent.com/3au4uk-1/CyBeer/master/version.json"
#define CYBEER_OTA_CHECK_CACHE_SEC 300
