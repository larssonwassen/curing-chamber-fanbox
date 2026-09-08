// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifdef __cplusplus
extern "C" {
#endif

#include "time.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_partition.h"
#include "esp_log.h"
#include <sys/time.h>

#define NANOSECONDS_PER_TICK (1000000000l / CONFIG_ESP32S3_DEFAULT_CPU_FREQ_MHZ)

portMUX_TYPE microsMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE nanosMux = portMUX_INITIALIZER_UNLOCKED;

void noInterrupts()
{
    portDISABLE_INTERRUPTS();
}

void interrupts()
{
    portENABLE_INTERRUPTS();
}

uint32_t IRAM_ATTR ticks()
{
    static unsigned long lccount = 0;
    static unsigned long overflow = 0;
    unsigned long ccount;
    portENTER_CRITICAL_ISR(&nanosMux);
    __asm__ __volatile__ ( "rsr     %0, ccount" : "=a" (ccount) );
    if(ccount < lccount)
    {
        overflow += UINT32_MAX;
    }
    lccount = ccount;
    portEXIT_CRITICAL_ISR(&nanosMux);
    return overflow + (ccount);
}

uint32_t IRAM_ATTR micros()
{
    static unsigned long lccount = 0;
    static unsigned long overflow = 0;
    unsigned long ccount;
    portENTER_CRITICAL_ISR(&microsMux);
    __asm__ __volatile__ ( "rsr     %0, ccount" : "=a" (ccount) );
    if(ccount < lccount)
    {
        overflow += UINT32_MAX / CONFIG_ESP32S3_DEFAULT_CPU_FREQ_MHZ;
    }
    lccount = ccount;
    portEXIT_CRITICAL_ISR(&microsMux);
    return overflow + (ccount / CONFIG_ESP32S3_DEFAULT_CPU_FREQ_MHZ);
}

uint32_t IRAM_ATTR millis()
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

void delay(uint32_t ms)
{
    if (ms < 10)
    {
        delayMicroseconds(ms * 1000);
    }
    else
    {
        vTaskDelay(ms / portTICK_PERIOD_MS);
    }
}

void IRAM_ATTR delayMicroseconds(uint32_t us)
{
    uint32_t m = micros();
    if(us){
        uint32_t e = (m + us);
        if(m > e){ //overflow
            while(micros() > e){
                NOP();
            }
        }
        while(micros() < e){
            NOP();
        }
    }
}
void IRAM_ATTR delayNanoseconds(uint32_t ns)
{
    uint32_t m = ticks();
    if(ns > 10)
    {
        uint32_t e = (m + (ns * NANOSECONDS_PER_TICK));
        if(m > e)
        { //overflow
            while(ticks() > e)
            {
                NOP();
            }
        }
        while(ticks() < e)
        {
            NOP();
        }
    }
}

#ifdef __cplusplus
}
#endif
