/*---------------------------------------------------------*\
| KeychronUltraController.h                               |
|                                                           |
|   Driver for the Keychron V6 Ultra 8K running custom ZMK  |
|   firmware with OpenRGB direct-control support (issue     |
|   #893). Speaks the id_openrgb (0x16) command over the    |
|   raw-HID channel (usage page 0xFF60), 32-byte reports.   |
|                                                           |
|   Wire format:  [0x00 reportid][0x16][sub][args...]       |
|     sub 0x00 GET_PROTOCOL_VERSION -> args[0]              |
|     sub 0x01 GET_LED_COUNT        -> args[0..1] LE        |
|     sub 0x02 SET_DIRECT_MODE      args[0]=1 on/0 off      |
|     sub 0x03 SET_LEDS  args[0]=start,args[1]=count,       |
|                        then count*3 RGB bytes (<=9/pkt)   |
|     sub 0x04 GET_LED_INFO args[0]=idx -> args[1..3]=x,y,fl|
\*---------------------------------------------------------*/

#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <hidapi.h>
#include "RGBController.h"

#define KEYCHRON_ULTRA_EPSIZE       32
#define KEYCHRON_ULTRA_LEDS_PER_PKT 9

/*---------------------------------------------------------*\
| Highest protocol version this plugin knows how to speak.   |
| The firmware reports its own version, so a newer board can |
| be turned away instead of being driven with the wrong wire |
| format.                                                    |
\*---------------------------------------------------------*/
#define KEYCHRON_ULTRA_PROTOCOL_VERSION 3

/*---------------------------------------------------------*\
| SET_LEDS and GET_LED_INFO carry the LED index in a single  |
| byte, so the protocol cannot address more than 256 LEDs.   |
| The biggest board today has 108.                           |
\*---------------------------------------------------------*/
#define KEYCHRON_ULTRA_MAX_LEDS     256

/*---------------------------------------------------------*\
| Shared with the plugin's detection pass: a reconnect must  |
| filter for exactly the same interface detection picked,    |
| so these live here rather than being duplicated.          |
\*---------------------------------------------------------*/
#define KEYCHRON_ULTRA_VID             0x3434
#define KEYCHRON_ULTRA_RAW_USAGE_PAGE  0xFF60
#define KEYCHRON_ULTRA_RAW_USAGE       0x61

/*---------------------------------------------------------*\
| Minimum gap between reconnect attempts. Without this, a    |
| genuinely unplugged keyboard would make every failed write |
| (12 packets per frame, many frames per second) re-run       |
| hid_enumerate and burn CPU for nothing.                    |
\*---------------------------------------------------------*/
#define KEYCHRON_ULTRA_RECONNECT_COOLDOWN_MS 2000

struct KeychronMixedEffect
{
    unsigned char effect;
    unsigned char hue;
    unsigned char saturation;
    unsigned char speed;
    unsigned char value;
    unsigned int  time;
};

class KeychronUltraController
{
public:
    KeychronUltraController(hid_device* dev_handle, const char* path,
                              unsigned short device_pid, unsigned int expected_led_count);
    ~KeychronUltraController();

    std::string    GetLocation();
    std::string    GetSerialString();
    unsigned int   GetLEDCount();          // queries the device; 0 = not our firmware
    unsigned int   GetProtocolVersion();   // 0 if the device did not answer

    void           SetDirectMode(bool enable);
    void           EnsureDirect();         // enter direct + start keepalive if not already
    void           SetLEDs(const std::vector<RGBColor>& colors);

    /* Firmware protocol v2: on-device Mixed RGB layers. */
    bool           SetMixedMode(bool enable);
    bool           GetMixedInfo(unsigned char& layers, unsigned char& effects_per_layer);
    bool           GetMixedRegions(unsigned int start, unsigned int count, std::vector<unsigned char>& masks);
    bool           SetMixedRegions(unsigned int start, const std::vector<unsigned char>& masks);
    bool           GetMixedEffect(unsigned char layer, unsigned char slot, KeychronMixedEffect& effect);
    bool           SetMixedEffect(unsigned char layer, unsigned char slot, const KeychronMixedEffect& effect);
    bool           SaveMixedSettings();
    bool           SetMasterBrightness(unsigned char brightness);

    /*-----------------------------------------------------------------------*\
    | Put the board back the way it looked before a rescan destroyed the old   |
    | controller. Does nothing if this is the first time we have seen the      |
    | board this run, so a fresh start leaves the onboard lighting alone       |
    | rather than blanking the keyboard.                                       |
    \*-----------------------------------------------------------------------*/
    void           RestoreLastFrame();
    bool           GetLEDPosition(unsigned int idx, unsigned int& x, unsigned int& y,
                                  unsigned int& flags);

    /*-----------------------------------------------------------------------*\
    | True (once) after a reconnect moved us to a different HID path, so the  |
    | RGBController can refresh the location string it cached at construction.|
    \*-----------------------------------------------------------------------*/
    bool           TakeLocationChanged();

private:
    hid_device*        dev;
    std::string        location;
    unsigned short     pid;               // for re-enumeration on reconnect
    unsigned int       expected_leds;     // reconnect must land on the same board
    std::mutex         hid_mutex;         // serialize HID I/O (OpenRGB thread + keepalive)
    std::mutex         state_mutex;       // serialize direct-mode / keepalive lifecycle

    std::atomic<bool>  direct_active;
    std::atomic<bool>  keepalive_run;
    std::atomic<bool>  location_changed;
    bool               shutting_down;     // guarded by hid_mutex; blocks reconnect in dtor
    std::chrono::steady_clock::time_point last_reconnect;   // guarded by hid_mutex
    std::thread        keepalive_thread;  // re-arms firmware watchdog while direct-mode is on

    /*-----------------------------------------------------------------------*\
    | xfer() takes hid_mutex and retries once through a reconnect. The        |
    | _locked variants assume the caller already holds hid_mutex, which is    |
    | what lets ReconnectLocked() re-take direct mode without recursing into  |
    | the non-recursive mutex (or inverting the state_mutex -> hid_mutex      |
    | order that SetDirectMode establishes).                                  |
    \*-----------------------------------------------------------------------*/
    int  xfer(const unsigned char* payload, size_t len, unsigned char* resp);
    int  xfer_locked(const unsigned char* payload, size_t len, unsigned char* resp);
    unsigned int GetLEDCountLocked();
    bool ReconnectLocked();
    void KeepaliveLoop();

    /*-----------------------------------------------------------------------*\
    | The last frame sent to each board, kept per PID so a controller built    |
    | after a rescan can pick up where the destroyed one left off. Static      |
    | because the controller object itself does not survive the rescan.        |
    \*-----------------------------------------------------------------------*/
    static std::mutex                                    frame_mutex;
    static std::map<unsigned short, std::vector<RGBColor> > last_frame;
};
