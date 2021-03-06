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
 * Socket like device access manager
 * This allows a generic paradigm (sockets) to be used to access 'device' instances (eg UART or I2C), with a standard set of methods + synchronisation
 * Devices must be created at init time directly, and they register with this manager to become accessible. Multiple app accesses are possible to the
 * same device instance.
 */
#include "os/os.h"
#include "wyres-generic/wutils.h"

#include "wyres-generic/wskt_driver.h"

#define MAX_WSKT_DEVICES MYNEWT_VAL(MAX_WSKT_DEVICES)
#define MAX_WSKTS MYNEWT_VAL(MAX_WSKTS)


// Registered devices that are accessed by wskt manager
static wskt_device_t _devices[MAX_WSKT_DEVICES];        // TODO should be a mempool
static uint8_t _devRegIdx = 0;

// Max simultaneous open sockets
static wskt_t _skts[MAX_WSKTS];         // TODO should be a mempool

// private fns
static wskt_device_t* findDeviceInst(const char* dname);
static wskt_t* allocSocket(wskt_device_t* dev);
static void freeSocket(wskt_t* s);

// DEVICE API
// To register devices at init
void wskt_registerDevice(const char* device_name, wskt_devicefns_t* dfns, void* dcfg) {
    assert(_devRegIdx<MAX_WSKT_DEVICES);
    assert(device_name!=NULL);
    // check if already created and assert, as this is an error (can't have same name and 2 different configs)
    for(int i=0;i<_devRegIdx;i++) {
        if (strncmp(device_name, _devices[i].dname, MAX_WKST_DNAME_SZ)==0) {
            // Can't log to uart in here, but can put it in buffer for debugger
            log_noout("double reg of wskt device %s",device_name);
            assert(0);      // badness in code
        }
    }
    wskt_device_t* dev = &_devices[_devRegIdx++];        // MEMPOOL
    strncpy(dev->dname, device_name, MAX_WKST_DNAME_SZ-1);
    dev->dname[MAX_WKST_DNAME_SZ-1]= '\0';
    dev->device_fns = dfns;
    dev->device_cfg = dcfg;
    return;
}
/**
 *  get open sockets on my device - caller gives an array of pointers of size bsz to copy them into
 * If sbuf==NULL then just return count of the open sockets
 * 
 */
uint8_t wskt_getOpenSockets(const char* device, wskt_t** sbuf, uint8_t bsz) {
    assert(device!=NULL);
    //Find those that match the device name
    int si=0;
    for(int i=0;i<MAX_WSKTS;i++) {
        if (_skts[i].dev!=NULL && strncmp(device, ((wskt_device_t*)(_skts[i].dev))->dname, MAX_WKST_DNAME_SZ)==0) {
            // Matching device name, add socket pointer to list (if given)
            if (sbuf!=NULL) {
                sbuf[si++] = &_skts[i];
                if (si>=bsz) {
                    // more matches than the size of the array you gave me...
                    return si;
                }
            } else {
                si++;       // just counting
            }
        }
    }
    return si;
}

// APP API : access devices via socket like ops
// open new socket to a device instance. If NULL rturned then the device is not accessible
// The evt must have its arg pointing to the correct thing for this device eg a buffer to receive into
wskt_t* wskt_open(const char* device_name, struct os_event* evt, struct os_eventq* eq) {
    assert(device_name!=NULL);
    // Find device
    wskt_device_t* dev = findDeviceInst(device_name);
    if (dev==NULL) {
        return NULL;
    }
    // Allocate new skt (and set the driver for it)
    wskt_t* ret = allocSocket(dev);
    if (ret!=NULL) {
        // save evt/eq into it
        ret->evt = evt;
        ret->eq = eq;
            
        // Tell driver to open
        if ((*(WSKT_DEVICE_FNS(ret))->open)(ret)<0) {
            // broken on open, free the socket
            freeSocket(ret);
            return NULL;
        }
    }
    return ret;
}
// configure specific actions on the device. Conflictual commands from multiple sockets are not advised... as far as possible they will mediated eg power off...
int wskt_ioctl(wskt_t* skt, wskt_ioctl_t* cmd) {
    assert(skt!=NULL);
    return (*(WSKT_DEVICE_FNS(skt))->ioctl)(skt, cmd);
}
// Send data to the device. This will be interleaved with other open sockets on the same device on a block basis
int wskt_write(wskt_t* skt, uint8_t* data, uint32_t sz) {
    assert(skt!=NULL);
    // Check have write access
    return (*(WSKT_DEVICE_FNS(skt))->write)(skt, data, sz);
}

// indicate done using this device. Your skt variable will be set to NULL after to avoid any unpleasentness
int wskt_close(wskt_t** skt, WSKT_CBFN_t cbfn) {
    assert(skt!=NULL);
    wskt_t*s = *skt;
    assert(s!=NULL);
    int ret = (*(WSKT_DEVICE_FNS(s))->close)(s);
    freeSocket(s);
    *skt = NULL;
    return ret;
}

// Internals

static wskt_device_t* findDeviceInst(const char* dname) {
    for(int i=0;i<MAX_WSKT_DEVICES;i++) {
        if ( _devices[i].dname!=NULL && strncmp(dname, _devices[i].dname, MAX_WKST_DNAME_SZ)==0) {
            return &_devices[i];
        }
    }
    return NULL;
}

static wskt_t* allocSocket(wskt_device_t* dev) {
    for(int i=0;i<MAX_WSKTS;i++) {
        if (_skts[i].dev==NULL) {
            _skts[i].dev = dev;       // yours now
            return &_skts[i];
        }
    }
    return NULL;

}
static void freeSocket(wskt_t* s) {
    s->dev = NULL;
}