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
/**
 * Sensor manager. 
 * Works on a start/stop basis, records the values which can be read at any time
 */
#include "os/os.h"
#include "bsp/bsp.h"
#include "hal/hal_i2c.h"

#include "wyres-generic/wutils.h"
#include "wyres-generic/timemgr.h"
#include "wyres-generic/gpiomgr.h"

#include "wyres-generic/sensorMgr.h"

// debug : must disable ext-button reading if using it for debug output
#if MYNEWT_VAL(UART_DBG)
#undef EXT_BUTTON
#define EXT_BUTTON (-1)
#endif

#define GPIO_ADC1   (-1)        // or EXT_IO is possible
#define CHAN_ADC1   (-1)
#define GPIO_ADC2   (-1)
#define CHAN_ADC2   (-1)

#define MAX_CBS (4)

// store values in between checks
static struct {
    SR_CBFN_t buttonCBs[MAX_CBS];
    SR_CBFN_t noiseCBs[MAX_CBS];
    uint32_t lastReadTS;
    uint32_t lastSignificantChangeTS;
    uint32_t lastButtonPressTS;
    uint8_t currButtonState;
    uint8_t lastButtonState;
    bool isActive;
    int16_t currTempdC;
    uint16_t currBattmV;
    uint32_t currPressurePa;
    uint8_t currLight;
    int16_t lastTempdC;
    uint16_t lastBattmV;
    uint32_t lastPressurePa;
    uint8_t lastLight;
    uint32_t lastNoiseTS;
    uint8_t noiseFreqkHz;
    uint8_t noiseLeveldB;
    uint16_t currADC1mV;
    uint16_t currADC2mV;
    uint16_t lastADC1mV;
    uint16_t lastADC2mV;
} _ctx = {
    .buttonCBs={NULL},
    .noiseCBs={NULL},
    .currButtonState=0,
    .lastButtonState=0,
    .isActive=false,
    .lastReadTS=0,
    .lastSignificantChangeTS=0,
    .lastButtonPressTS=0,
};

// Timeout for I2C accesses in 'ticks'
#define I2C_ACCESS_TIMEOUT (100)

static void buttonCB(void* arg);
static void config();
    // read stuff into current values
static void readEnv();
static void deconfig();
static uint32_t delta(int a, int b);

void SRMgr_start() {
    // configure GPIOs / I2C for periphs
    config();
    _ctx.isActive = true;    
    // read stuff into current values
    readEnv();
}

void SRMgr_stop() {
    // read stuff into current values before stopping
    readEnv();
    _ctx.isActive = false;    
    // deconfigure GPIOS/I2C
    deconfig();
}

bool SRMgr_registerButtonCB(SR_CBFN_t cb) {
    if (EXT_BUTTON>=0) {
        for(int i=0;i<MAX_CBS;i++) {
            if (_ctx.buttonCBs[i]==NULL) {
                _ctx.buttonCBs[i] = cb;
                return true;
            }
        }
    }
    return false;       // no space or no button defined
}
void SRMgr_unregisterButtonCB(SR_CBFN_t cb) {
    for(int i=0;i<MAX_CBS;i++) {
        if (_ctx.buttonCBs[i]==cb) {
            _ctx.buttonCBs[i] = NULL;
        }
    }
}

// Register callback to be notified when noise is detected (also means micro is active during deep sleep)
bool SRMgr_registerNoiseCB(SR_CBFN_t cb) {
    // Activate microphone stuff TODO
    for(int i=0;i<MAX_CBS;i++) {
        if (_ctx.noiseCBs[i]==NULL) {
            _ctx.noiseCBs[i] = cb;
            return true;
        }
    }
    return false;       // no space
}
// Remove registration - if noone is registered then micro input only checked at UL time..
void SRMgr_unregisterNoiseCB(SR_CBFN_t cb) {
    bool atLeastOneCB = false;
    for(int i=0;i<MAX_CBS;i++) {
        if (_ctx.noiseCBs[i]==cb) {
            _ctx.noiseCBs[i] = NULL;
        }
        if (_ctx.noiseCBs[i]!=NULL) {
            atLeastOneCB = true;
        }
    }
    // If no CBs disable noise check task
    if (atLeastOneCB) {
        // TODO
    }
}

uint32_t SRMgr_getLastEnvChangeTime() {
    return _ctx.lastSignificantChangeTS;
}
bool SRMgr_hasButtonChanged() {
    return (_ctx.currButtonState != _ctx.lastButtonState);
}
uint8_t SRMgr_getButton() {
    readEnv();
    return _ctx.currButtonState;
}
bool SRMgr_hasTempChanged() {
    return (delta(_ctx.currTempdC, _ctx.lastTempdC)>2);
}
int16_t SRMgr_getTempdC() {
    readEnv();
    return _ctx.currTempdC;        // value in 1/10 C
}
bool SRMgr_hasPressureChanged() {
    return (delta(_ctx.currPressurePa, _ctx.lastPressurePa)>10);
}
uint32_t SRMgr_getPressurePa() {
    readEnv();
    return _ctx.currPressurePa;       // in Pa
}
bool SRMgr_hasBattChanged() {
    return (delta(_ctx.currBattmV, _ctx.lastBattmV)>50);
}
uint16_t SRMgr_getBatterymV() {
    readEnv();
    return _ctx.currBattmV;         // in mV
}
bool SRMgr_hasLightChanged() {
    return (delta(_ctx.currLight, _ctx.lastLight)>2);
}
uint8_t SRMgr_getLight() {
    readEnv();
    return _ctx.currLight;
}
uint32_t SRMgr_getLastButtonTime() {
    return _ctx.lastButtonPressTS;
}
uint32_t SRMgr_getLastNoiseTime() {
    return _ctx.lastNoiseTS;
}
uint8_t SRMgr_getNoiseFreqkHz() {
    return _ctx.noiseFreqkHz;
}
uint8_t SRMgr_getNoiseLeveldB() {
    return _ctx.noiseLeveldB;
}

bool SRMgr_hasADC1Changed() {
    return (delta(_ctx.currADC1mV, _ctx.lastADC1mV)>50);
}
uint16_t SRMgr_getADC1mV() {
    readEnv();
    return _ctx.currADC1mV;
}
bool SRMgr_hasADC2Changed() {
    return (delta(_ctx.currADC2mV, _ctx.lastADC2mV)>50);
}
uint16_t SRMgr_getADC2mV() {
    readEnv();
    return _ctx.currADC2mV;
}
// Any value that has changed 'significantly' has the last value updated to the current 
// app layer can decide to do this after having read and sent values that had changed
bool SRMgr_updateEnvs() {
    bool changed = false;
    // only update those that have changed (otherwise slowly changing values will never be seen as changed)
    if (SRMgr_hasBattChanged()) {
        _ctx.lastBattmV = _ctx.currBattmV;
        changed = true;
    }
    if (SRMgr_hasLightChanged()) {
        _ctx.lastLight = _ctx.currLight;
        changed = true;
    }
    if (SRMgr_hasTempChanged()) {
        _ctx.lastTempdC = _ctx.currTempdC;
        changed = true;
    }
    if (SRMgr_hasPressureChanged()) {
        _ctx.lastPressurePa = _ctx.currPressurePa;
        changed = true;
    }
    if (SRMgr_hasADC1Changed()) {
        _ctx.lastADC1mV = _ctx.currADC1mV;
        changed = true;
    }
    if (SRMgr_hasADC2Changed()) {
        _ctx.lastADC2mV = _ctx.currADC2mV;
        changed = true;
    }
    if (SRMgr_hasButtonChanged()) {
        _ctx.lastButtonState = _ctx.currButtonState;
        changed = true;
    }
    if (changed) {
        _ctx.lastSignificantChangeTS = TMMgr_getRelTime();
    }
    return changed;
}
// internals
static void buttonCB(void* arg) {
    _ctx.lastButtonState = _ctx.currButtonState;
    _ctx.currButtonState = GPIO_read(EXT_BUTTON);
    if (_ctx.currButtonState != _ctx.lastButtonState) {
        _ctx.lastButtonPressTS = TMMgr_getRelTime();
        // call callbacks
        for(int i=0;i<MAX_CBS;i++) {
            if (_ctx.buttonCBs[i]!=NULL) {
                (*(_ctx.buttonCBs[i]))();
            }
        }
    }
}
static void config() {
    // config GPIOS
    if (EXT_BUTTON>=0) {
        GPIO_define_irq("button", EXT_BUTTON, buttonCB, &_ctx, HAL_GPIO_TRIG_BOTH, HAL_GPIO_PULL_UP, LP_DEEPSLEEP);
    }
    // Note the ADC ones will work but return 0 on read if ADC not enabled
    if (LIGHT_SENSOR>=0) {
        GPIO_define_adc("light", LIGHT_SENSOR, LIGHT_SENSOR_ADCCHAN, LP_DOZE);
        log_debug("S adc-light");
    }
    if (GPIO_ADC1>=0) {
        GPIO_define_adc("adc1", GPIO_ADC1, CHAN_ADC1, LP_DOZE);
    }
    if (GPIO_ADC2>=0) {
        GPIO_define_adc("adc2", GPIO_ADC2, CHAN_ADC2, LP_DOZE);
    }
    if (BATTERY_GPIO>=0) {
        GPIO_define_adc("battery", BATTERY_GPIO, BATTERY_ADCCHAN, LP_DOZE);
        log_debug("S adc-batt");
    }
    // config alti on i2c
    // TODO
    // config noise detector on micro
    // TODO
}


// read stuff into current values
static void readEnv() {
    if (_ctx.isActive) {
        _ctx.lastReadTS = TMMgr_getRelTime();

        if (EXT_BUTTON>=0) {
            _ctx.currButtonState = GPIO_read(EXT_BUTTON);
        }
        if (BATTERY_GPIO>=0) {
            _ctx.currBattmV = GPIO_readADCmV(BATTERY_GPIO);
            log_debug("S bat %d", _ctx.currBattmV);
        }
        if (LIGHT_SENSOR>=0) {
            _ctx.currLight = GPIO_readADCmV(LIGHT_SENSOR)/16;  // 12 bit value, divide down to give a 8 bit value
            log_debug("S lum %d", _ctx.currLight);
        }
        if (GPIO_ADC1>=0) {
            _ctx.currADC1mV = GPIO_readADCmV(GPIO_ADC1);
        }
        if (GPIO_ADC2>=0) {
            _ctx.currADC2mV = GPIO_readADCmV(GPIO_ADC2);
        }
    }
}


static void deconfig() {
    // remove config GPIOS
    if (EXT_BUTTON>=0) {
        GPIO_release(EXT_BUTTON);
    }
    if (LIGHT_SENSOR>=0) {
        GPIO_release(LIGHT_SENSOR);
    }
    if (GPIO_ADC1>=0) {
        GPIO_release(GPIO_ADC1);
    }
    if (GPIO_ADC2>=0) {
        GPIO_release(GPIO_ADC2);
    }
    if (BATTERY_GPIO>=0) {
        GPIO_release(BATTERY_GPIO);
    }
    // config alti on i2c
    // TODO
    // config noise detector on micro
    // TODO
}

static uint32_t delta(int a, int b) {
    if (a>b) {
        return (uint32_t)(a-b);
    }
    return (uint32_t)(b-a);
}