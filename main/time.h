#ifndef TIME_H_
#define TIME_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <math.h>
#include "sdkconfig.h"

#ifndef F_CPU
#define F_CPU (CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ * 1000000U)
#endif

#define ESP_REG(addr) *((volatile uint32_t *)(addr))
#define NOP() asm volatile ("nop")

uint32_t ticks();
uint32_t micros();
uint32_t millis();
void delay(uint32_t);
void delayMicroseconds(uint32_t us);
void delayNanoseconds(uint32_t ns);
void noInterrupts();
void interrupts();

#ifdef __cplusplus
}
#endif

#endif /* TIME_H_ */