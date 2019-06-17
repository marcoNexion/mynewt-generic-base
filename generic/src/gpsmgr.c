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


#include "os/os.h"
#include "wyres-generic/wutils.h"
#include "wyres-generic/wskt_user.h"
#include "wyres-generic/gpiomgr.h"
#include "wyres-generic/gpsmgr.h"
#include "wyres-generic/lowpowermgr.h"
#include "wyres-generic/uartSelector.h"
#include "wyres-generic/timemgr.h"
#include "wyres-generic/minmea.h"

#define GPS_UART    MYNEWT_VAL(GPS_UART)
#define GPS_TASK_PRIO       MYNEWT_VAL(GPS_TASK_PRIO)
#define GPS_TASK_STACK_SZ   OS_STACK_ALIGN(256)

static os_stack_t _gps_task_stack[GPS_TASK_STACK_SZ];
static struct os_task gps_mgr_task_str;

// L96 GPS commands    
static char* EASY_ON="$PMTK869,1,1*35\r\n";
static char* HOT_START="$PMTK101*32\r\n";
static char* COLD_START="$PMTK103*30\r\n";
// Standby mode is 500uA, but can be exited by uart data...
static char* STANDBY_MODE="$PMTK161,0*28\r\n";

static struct {
    struct os_event myGPSEvent;
    struct os_eventq gpsMgrEQ;
    struct os_mutex dataMutex;

    const char* uartDevice;
    int8_t pwrPin;
    int8_t uartSelect;
    wskt_t* cnx;
    uint8_t rxbuf[WSKT_BUF_SZ+1];
    gps_data_t gpsData;
    uint32_t lastFixTime;
    GPS_CB_FN_t cbfn;
    bool commOk;
} _ctx = {
    .lastFixTime = 0,
    .gpsData = {
        .prec = -1,
        .lat = 0,
        .lon = 0,
        .alt = 0,
        .rxAt = 0,
    },
    .cbfn=NULL,
};


// predeclare privates
static void gps_mgr_task(void* arg);
static void gps_mgr_rxcb(struct os_event* ev);
static bool parseNEMA(const char* line, gps_data_t* nd);

// TODO could refactor as a state machine...

// Called from appinit 
void gps_mgr_init(const char* dname, int8_t pwrPin, int8_t uartSelect) {
    _ctx.uartDevice = dname;
    _ctx.uartSelect=uartSelect;
    _ctx.pwrPin = pwrPin;
    if (_ctx.pwrPin>=0) {
        // Note 1 is OFF so start with it off
        GPIO_define_out("gpspower", _ctx.pwrPin, 1, LP_DEEPSLEEP);
    }
    // mutex for data access
    os_mutex_init(&_ctx.dataMutex);

    // Create eventQ
    os_eventq_init(&_ctx.gpsMgrEQ);
    // create event with arg pointing to our line buffer
    // TODO how to tell driver limit of size of buffer???
    _ctx.myGPSEvent.ev_cb = gps_mgr_rxcb;
    _ctx.myGPSEvent.ev_arg = _ctx.rxbuf;
    // Create task 
    os_task_init(&gps_mgr_task_str, "gps_task", gps_mgr_task, NULL, GPS_TASK_PRIO,
               OS_WAIT_FOREVER, _gps_task_stack, GPS_TASK_STACK_SZ);
}

bool getGPSData(gps_data_t* d) {
    bool ret = false;
    memset(d, 0, sizeof(gps_data_t));
        // mutex lock
    os_mutex_pend(&_ctx.dataMutex, OS_TIMEOUT_NEVER);
    if (_ctx.gpsData.rxAt>0) {
        ret = true;
        d->lat = _ctx.gpsData.lat;
        d->lon = _ctx.gpsData.lon;
        d->alt = _ctx.gpsData.alt;
        d->prec = _ctx.gpsData.prec;
        d->nSats = _ctx.gpsData.nSats;
        d->rxAt = _ctx.gpsData.rxAt;
    }
    // ok
    os_mutex_release(&_ctx.dataMutex);

    return ret;
}
// Get age of the last fix we got, or -1 if never had a fix
int32_t gps_lastGPSFixAgeMins() {
    if (_ctx.gpsData.rxAt>0) {
        return (TMMgr_getRelTime() - _ctx.gpsData.rxAt)/(60*1000);
    } else {
        return -1;
    }
}

// TODO Should be a SM?
void gps_start(GPS_CB_FN_t cbfn) {
    _ctx.cbfn = cbfn;
    _ctx.commOk = false;        // just starting...
    // Power up using power pin
    if (_ctx.pwrPin>=0) {
        log_debug("gps power ON using pin %d", _ctx.pwrPin);
        GPIO_write(_ctx.pwrPin, 0);     // yup pull down for ON
    } else {
        log_debug("gps poweralways on?");
    }
    // Select it as UART device (if required)
    if (_ctx.uartSelect>=0) {
        uart_select(_ctx.uartSelect);
    }
    // initialise comms to the gps via the uart like comms device defined in syscfg
    _ctx.cnx = wskt_open(_ctx.uartDevice, &_ctx.myGPSEvent, &_ctx.gpsMgrEQ);
    assert(_ctx.cnx!=NULL);
    // Set baud rate
    wskt_ioctl_t cmd = {
        .cmd = IOCTL_SET_BAUD,
        .param = MYNEWT_VAL(GPS_UART_BAUDRATE),
    };
    wskt_ioctl(_ctx.cnx, &cmd);
    wskt_write(_ctx.cnx, (uint8_t*)EASY_ON, strlen(EASY_ON));
    if (gps_lastGPSFixAgeMins() < 0 || gps_lastGPSFixAgeMins()>3*60) {
        wskt_write(_ctx.cnx, (uint8_t*)COLD_START, strlen(COLD_START));
    } else {
        wskt_write(_ctx.cnx, (uint8_t*)HOT_START, strlen(HOT_START));
    }
}
void gps_stop() {
    if (_ctx.pwrPin>=0) {
//        log_debug("gps power OFF using pin %d", _ctx.pwrPin);
//        GPIO_write(_ctx.pwrPin, 1);
        log_debug("gps power LEFT ON using pin %d", _ctx.pwrPin);
    } else {
        wskt_write(_ctx.cnx, (uint8_t*)STANDBY_MODE, strlen(STANDBY_MODE));
    }
    if (_ctx.cnx!=NULL) {
        wskt_close(&_ctx.cnx);      // should take effect after empty of current tx buffer...
    }
    _ctx.cbfn = NULL;
}

// task just runs the callbacks
static void gps_mgr_task(void* arg) {
    while(1) {
        os_eventq_run(&_ctx.gpsMgrEQ);
    }
}

// callback every time the socket gives us a new line of data from the GPS
static void gps_mgr_rxcb(struct os_event* ev) {
    // ev->arg is our line buffer
    const char* line = (char*)(ev->ev_arg);
    assert(line!=NULL);
    if (strnlen(line, 10)<10) {
        // too short line ignore
        return;
    }
    // parse it
    gps_data_t newdata;
    // if unparseable then tell cb
    if (!parseNEMA(line, &newdata)) {
        log_debug("bad gps line [%s]", line);
        if (_ctx.commOk) {
            _ctx.commOk = false;
            if (_ctx.cbfn!=NULL) {
                (*_ctx.cbfn)(GPS_COMM_FAIL);
            }
        }
        return;
    }
//    log_debug("gps [%s]", line);

    // update current position if got one
    if (newdata.prec>0) {
        log_debug("new gps data ok (%d, %d, %d) (%d) (%d)", newdata.lat, newdata.lon, newdata.alt, newdata.prec, newdata.nSats);
        // mutex lock
        os_mutex_pend(&_ctx.dataMutex, OS_TIMEOUT_NEVER);
        _ctx.gpsData.lat = newdata.lat;
        _ctx.gpsData.lon = newdata.lon;
        _ctx.gpsData.alt = newdata.alt;
        _ctx.gpsData.prec = newdata.prec;
        _ctx.gpsData.nSats = newdata.nSats;
        _ctx.gpsData.rxAt = TMMgr_getRelTime();
        // ok
        os_mutex_release(&_ctx.dataMutex);

        // tell cb
        if (_ctx.cbfn!=NULL) {
            (*_ctx.cbfn)(GPS_NEWFIX);
        }
    } else {
        if (!_ctx.commOk) {
            _ctx.commOk = true;
            if (_ctx.cbfn!=NULL) {
                (*_ctx.cbfn)(GPS_COMM_OK);
            }
        }
    }
    // and continue
}

// Returns true if parsed ok, false if unparseable.
// sets the 'prec' to 0 if no location data extracted
static bool parseNEMA(const char* line, gps_data_t* nd) {
    if (!minmea_check(line, true)) {
        return false;
    }
    int8_t si = minmea_sentence_id(line, true);
    switch(si) {
        case MINMEA_SENTENCE_GGA: {
            struct minmea_sentence_gga ggadata;
            if (minmea_parse_gga(&ggadata, line)) {
                if (ggadata.fix_quality>0) {
                    nd->lat = ggadata.latitude.value;
                    nd->lon = ggadata.longitude.value;
                    nd->alt = ggadata.altitude.value;
                    nd->nSats = ggadata.satellites_tracked;
                    nd->prec = ggadata.hdop.value*2; // precision diameter for 95% is 50% *2? 
                    if (nd->prec < 1) {
                        nd->prec = 5;
                    }
                    log_debug("gga ok + fix");
                } else {
                    log_debug("gga ok no fix");
                    nd->prec = 0;
                }
            } else {
                // hmmm not so good
                log_debug("gga bad");
                nd->prec = 0;
            }
            return true;
        }
        case MINMEA_INVALID: {
            return false;
        }
        case MINMEA_SENTENCE_RMC: {
            struct minmea_sentence_rmc rmcdata;
            if (minmea_parse_rmc(&rmcdata, line)) {
                if (rmcdata.valid) {
                    log_debug("rmc ok + fix");
                } else {
                    log_debug("rmc ok no fix");
                }
            } else {
                log_debug("rmc nok");
            }
            // could be useful but GGA has more data - ignore
            nd->prec = 0;
            return true;
        }
        default: {
            // ignore, but not an error
            nd->prec = 0;
            log_debug("gps[%d][%c%c%c%c%c%c]", si, line[0],line[1],line[2],line[3],line[4],line[5]);
            return true;
        }
    }
    // never get here
}
#ifdef UNITTEST
bool unittest_gps() {
    // test NEMA parsing
    gps_data_t newdata;
    bool ret = true;        // assume all will go ok
    // Try bad nemas
    ret &= unittest("empty line", !parseNEMA("", &newdata));
    ret &= unittest("poorly formated", !parseNEMA("", &newdata));
    ret &= unittest("bad CRC", !parseNEMA("", &newdata));
    // GGA but bad
    ret &= unittest("GGA bad CRC", !parseNEMA("$GNGGA,143547.00,4511.10189,N,00542.33219,E,1,09,2.93,193.7,M,47.4,M,,*00", &newdata));
    ret &= unittest("GGA no data", parseNEMA("$GNGGA,093321.00,,,,,0,05,58.77,,,,,,*7A", &newdata));
    ret &= unittest("GGA no data result", newdata.prec==0);
    // Try good but unuseful ones - these return OK for parse but data is prec==-1
    ret &= unittest("GNGLL", parseNEMA("$GNGLL,4511.10224,N,00542.33211,E,143546.00,A,A*73", &newdata));
    ret &= unittest("GNGLL no result", newdata.prec==0);
    ret &= unittest("GNRMC", parseNEMA("$GNRMC,143547.00,A,4511.10189,N,00542.33219,E,0.140,,220318,,,A*68", &newdata));
    ret &= unittest("GNRMC no result", newdata.prec==0);
    ret &= unittest("GNGSA", parseNEMA("$GNGSA,A,3,74,75,83,65,,,,,,,,,3.73,2.93,2.30*1B", &newdata));
    ret &= unittest("GNGSA no result", newdata.prec==0);
    ret &= unittest("GPGSV", parseNEMA("$GPGSV,3,1,12,02,24,104,21,06,21,065,,12,72,031,33,14,24,313,25*7D", &newdata));
    ret &= unittest("GPGSV no result", newdata.prec==0);
    ret &= unittest("GLGSV", parseNEMA("$GLGSV,3,1,09,65,24,283,19,66,11,341,,72,12,237,,73,27,093,*67", &newdata));
    ret &= unittest("GLGSV no result", newdata.prec==0);
    // good ones - test the parse passes and the data is as expected
    ret &= unittest("GGA basic", parseNEMA("$GNGGA,143547.00,4511.10189,N,00542.33219,E,1,09,2.93,193.7,M,47.4,M,,*41", &newdata));
    ret &= unittest("GGA basic", newdata.prec>0 && newdata.lat==451110189 && newdata.lon==54233219 && newdata.alt==1937);
    return ret;
}
#endif /* UNITTEST */
