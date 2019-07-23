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
#ifndef H_ACC_BASIC_H
#define H_ACC_BASIC_H

#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

// interface to accelero - rewrite to use sensor device driver?
bool ACC_init();
bool ACC_activate();
bool ACC_sleep();
bool ACC_readXYZ(int8_t* xp, int8_t* yp, int8_t* zp);
bool ACC_HasDetectedMoved(void);
bool ACC_HasDetectedFalling(void);

#ifdef __cplusplus
}
#endif

#endif  /* H_ACC_BASIC_H */