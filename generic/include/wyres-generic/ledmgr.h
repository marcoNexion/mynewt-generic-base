/**
 * Copyright 2019 Wyres
 * Licensed under the Apache License, Version 2.0 (the "License"); 
 * you may not use this file except in compliance with the License. 
 * You may obtain a copy of the License at
 *    http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, 
 * software distributed under the License is distributed on 
 * an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, 
 * either express or implied. See the License for the specific 
 * language governing permissions and limitations under the License.
*/
#ifndef H_LEDMGR_H
#define H_LEDMGR_H

#include <inttypes.h>
#include <mcu/mcu.h>

#ifdef __cplusplus
extern "C" {
#endif

// ledmgr allows multi module access to N LEDs, which can be flashed in a 2 second repeating pattern specified by a 20 character string.
// Each '1' means on, and '0' is off for 100ms. Configuration of the pattern length and slice time can be changed using the defines.
// A pattern is requested for a specific LED for a specific time, and can also be cancelled before the end of the requested duration.
// api
// values for the pri parameter to either queue the request or override the current one
typedef enum { LED_REQ_ENQUEUE, LED_REQ_INTERUPT} LED_PRI;

// Request for the given LED 'gpio' to flash 'pattern', for 'dur' seconds, either interuppting the current request (if any) or enqueuing behind it
// Returns true if the request was accepted, or false if the queue was full.
bool ledRequest(int8_t gpio, const char* pattern, uint32_t dur, LED_PRI pri);
// execute led pattern immediate (interupting any executing currently)
bool ledStart(int8_t gpio, const char* pattern, uint32_t dur);
// Cancel the current pattern flashing on the given gpio (if another is queued behind it, it becomes the current one)
void ledCancel(int8_t gpio);

// Some common flash patterns
#define FLASH_MIN  ("10000000000000000000")       
#define FLASH_05HZ  ("11111111110000000000")       
#define FLASH_1HZ  ("11111000001111100000")       
#define FLASH_2HZ ("11000110001100011000")
#define FLASH_5HZ ("10101010101010101010")
#define FLASH_ON  ("11111111111111111111")       


#ifdef __cplusplus
}
#endif

#endif  /* H_LEDMGR_H */
