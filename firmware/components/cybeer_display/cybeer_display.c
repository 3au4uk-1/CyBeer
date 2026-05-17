#include "cybeer_display.h"

#include "cybeer_config.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_rom_sys.h"

#define TM1637_CMD_DATA_AUTO 0x40
#define ADDR_SEG0           0xC0

#define DIGIT_COLON_BIT 0x80
#define BIT_TIME_US      10

static const gpio_num_t s_clk = (gpio_num_t)CYBEER_GPIO_TM1637_CLK;
static const gpio_num_t s_dio = (gpio_num_t)CYBEER_GPIO_TM1637_DIO;

static const uint8_t s_digit_seg[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66,
    0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};

static inline void bit_delay(void)
{
    esp_rom_delay_us(BIT_TIME_US);
}

static void dio_drive(bool high)
{
    gpio_set_direction(s_dio, GPIO_MODE_OUTPUT_OD);
    gpio_set_pull_mode(s_dio, GPIO_PULLUP_ENABLE);
    gpio_set_level(s_dio, high ? 1 : 0);
}

static void start(void)
{
    dio_drive(true);
    gpio_set_level(s_clk, 1);
    bit_delay();
    dio_drive(false);
}

static void stop(void)
{
    gpio_set_level(s_clk, 0);
    bit_delay();
    dio_drive(false);
    bit_delay();
    gpio_set_level(s_clk, 1);
    bit_delay();
    dio_drive(true);
}

static void write_byte(uint8_t byte)
{
    for (unsigned i = 0U; i < 8U; ++i) {
        gpio_set_level(s_clk, 0);
        dio_drive((byte & 1U) != 0);
        bit_delay();
        gpio_set_level(s_clk, 1);
        bit_delay();
        byte >>= 1;
    }

    gpio_set_level(s_clk, 0);
    bit_delay();
    gpio_set_pull_mode(s_dio, GPIO_PULLUP_ONLY);
    gpio_set_direction(s_dio, GPIO_MODE_INPUT);
    bit_delay();
    gpio_set_level(s_clk, 1);
    bit_delay();
    gpio_set_level(s_clk, 0);
    bit_delay();
}

static void flush_digits(const uint8_t digits[4])
{
    start();
    write_byte(TM1637_CMD_DATA_AUTO);
    stop();

    start();
    write_byte(ADDR_SEG0);
    for (unsigned i = 0U; i < 4U; ++i) {
        write_byte(digits[i]);
    }
    stop();

    start();
    write_byte(0x8F);
    stop();
}

void cybeer_display_init(void)
{
    const uint64_t mask = (1ULL << CYBEER_GPIO_TM1637_CLK) | (1ULL << CYBEER_GPIO_TM1637_DIO);
    const gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    gpio_set_level(s_clk, 1);
    gpio_set_level(s_dio, 1);

    cybeer_display_show_zeros();
}

static void digits_ss_cc(int64_t cs_in, uint8_t out[4])
{
    if (cs_in < 0) {
        cs_in = 0;
    }
    if (cs_in > 999999) {
        cs_in = 999999;
    }

    long cs = (long)cs_in;
    long sec = cs / 100L;
    int cc = (int)(cs % 100);
    if (sec > 99L) {
        sec = 99L;
    }

    unsigned s01 = (unsigned)(sec % 10L);
    unsigned s10 = (unsigned)(sec / 10L);
    unsigned c01 = (unsigned)(cc % 10);
    unsigned c10 = (unsigned)(cc / 10);

    out[0] = s_digit_seg[s10];
    out[1] = (uint8_t)(s_digit_seg[s01] | DIGIT_COLON_BIT);
    out[2] = s_digit_seg[c10];
    out[3] = s_digit_seg[c01];
}

void cybeer_display_show_us(int64_t us)
{
    int64_t cs = us / 10000LL;
    uint8_t d[4];
    digits_ss_cc(cs, d);
    flush_digits(d);
}

void cybeer_display_show_zeros(void)
{
    uint8_t d[4] = {
        s_digit_seg[0],
        (uint8_t)(s_digit_seg[0] | DIGIT_COLON_BIT),
        s_digit_seg[0],
        s_digit_seg[0],
    };
    flush_digits(d);
}
