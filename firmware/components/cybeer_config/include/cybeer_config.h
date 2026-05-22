#pragma once

#define CYBEER_GPIO_SWITCH      9
#define CYBEER_GPIO_TM1637_CLK  6
#define CYBEER_GPIO_TM1637_DIO  7
#define CYBEER_GPIO_LED_DATA    8
#define CYBEER_GPIO_BATTERY_ADC 0

#define CYBEER_DEBOUNCE_MS          40
#define CYBEER_STABLE_ON_MS         200
#define CYBEER_STABLE_OFF_MS        150

#define CYBEER_LED_COUNT_MAX        64
#define CYBEER_LED_COUNT_DEFAULT    20
#define CYBEER_LED_BRIGHTNESS_DEFAULT 64

#define CYBEER_ADMIN_PIN_MAX_LEN    16
#define CYBEER_MDNS_HOSTNAME        "cybeer"

#define CYBEER_OTA_MANIFEST_URL "https://raw.githubusercontent.com/zau/CyBeer/main/version.json"
#define CYBEER_OTA_CHECK_CACHE_SEC 300
