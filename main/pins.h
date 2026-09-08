#ifndef Pins_h
#define Pins_h

#include <stdint.h>
#include "driver/gpio.h"   // gpio_num_t, GPIO_NUM_*

// Silkscreen pin names for the ESP32-S3-Nano board, mapped to GPIO numbers.
//
// A0-A5 and D0-D9 match the Arduino Nano ESP32 mapping, which is what the two
// previously duplicated entries were resolved against:
//   - A3 was GPIO_NUM_2, the same as A1. It is GPIO 4.
//   - D10 was GPIO_NUM_18, the same as D9. It is GPIO 21.
// Neither is used by this firmware, and neither has been checked against the
// board with a meter -- but a definition that collides with another one is
// wrong however the board is wired.
static const gpio_num_t A0  = GPIO_NUM_1;
static const gpio_num_t A1  = GPIO_NUM_2;
static const gpio_num_t A2  = GPIO_NUM_3;
static const gpio_num_t A3  = GPIO_NUM_4;
static const gpio_num_t A4  = GPIO_NUM_11;
static const gpio_num_t A5  = GPIO_NUM_12;
// A6/A7 do not match the Arduino Nano ESP32 mapping (GPIO 13/14) and, more to
// the point, GPIO 33-37 carry the octal PSRAM this build now enables. Do not
// use these two without checking the board first.
static const gpio_num_t A6  = GPIO_NUM_34;
static const gpio_num_t A7  = GPIO_NUM_35;

static const gpio_num_t B0  = GPIO_NUM_46;
static const gpio_num_t B1  = GPIO_NUM_0;

static const gpio_num_t D0  = GPIO_NUM_44;
static const gpio_num_t D1  = GPIO_NUM_43;
static const gpio_num_t D2  = GPIO_NUM_5;
static const gpio_num_t D3  = GPIO_NUM_6;
static const gpio_num_t D4  = GPIO_NUM_7;
static const gpio_num_t D5  = GPIO_NUM_8;
static const gpio_num_t D6  = GPIO_NUM_9;
static const gpio_num_t D7  = GPIO_NUM_10;
static const gpio_num_t D8  = GPIO_NUM_17;
static const gpio_num_t D9  = GPIO_NUM_18;
static const gpio_num_t D10 = GPIO_NUM_21;
static const gpio_num_t D11 = GPIO_NUM_38;
static const gpio_num_t D12 = GPIO_NUM_47;
static const gpio_num_t D13 = GPIO_NUM_48;


static const gpio_num_t TX = D0;
static const gpio_num_t RX = D1;

static const gpio_num_t SDA = A4;
static const gpio_num_t SCL = A5;

static const gpio_num_t MOSI  = D11;
static const gpio_num_t MISO  = D12;
static const gpio_num_t SCK   = D13;


#endif /* Pins_h */
