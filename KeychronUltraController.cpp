/*---------------------------------------------------------*\
| KeychronUltraController.cpp                             |
|   Talks the custom-firmware OpenRGB command (0x16) over   |
|   the raw-HID channel (usage 0xFF60), 32-byte reports,    |
|   unnumbered report id (0x00 prefix on write).            |
\*---------------------------------------------------------*/

#include "KeychronUltraController.h"
#include <chrono>
#include <cstring>

/* id_openrgb (0x16) subcommands. Must match app/src/launcher/openrgb.c */
#define OPENRGB_CMD              0x16
#define SUB_GET_PROTOCOL_VERSION 0x00
#define SUB_GET_LED_COUNT        0x01
#define SUB_SET_DIRECT_MODE      0x02
#define SUB_SET_LEDS             0x03
#define SUB_GET_LED_INFO         0x04
#define SUB_MIXED_GET_INFO       0x10
#define SUB_MIXED_GET_REGIONS    0x11
#define SUB_MIXED_SET_REGIONS    0x12
#define SUB_MIXED_GET_EFFECTS   0x13
#define SUB_MIXED_SET_EFFECTS   0x14
#define SUB_SET_MIXED_MODE      0x15
#define SUB_MIXED_SAVE          0x16
#define SUB_SET_MASTER_BRIGHTNESS 0x17

#define KEEPALIVE_INTERVAL_MS    1500

std::mutex                                    KeychronUltraController::frame_mutex;
std::map<unsigned short, std::vector<RGBColor> > KeychronUltraController::last_frame;

KeychronUltraController::KeychronUltraController(hid_device* dev_handle, const char* path,
                                                     unsigned short device_pid,
                                                     unsigned int expected_led_count)
{
    dev              = dev_handle;
    location         = path;
    pid              = device_pid;
    expected_leds    = expected_led_count;
    direct_active    = false;
    keepalive_run    = false;
    location_changed = false;
    shutting_down    = false;

    /* Backdate so the first failure may reconnect immediately. */
    last_reconnect   = std::chrono::steady_clock::now()
                     - std::chrono::milliseconds(KEYCHRON_ULTRA_RECONNECT_COOLDOWN_MS);
}

KeychronUltraController::~KeychronUltraController()
{
    keepalive_run = false;
    if(keepalive_thread.joinable())
    {
        keepalive_thread.join();
    }

    /*-----------------------------------------------------------------------*\
    | Take the lock and go through xfer_locked, not xfer: a failing write here |
    | must not kick off a reconnect while the object is being destroyed.       |
    \*-----------------------------------------------------------------------*/
    std::lock_guard<std::mutex> lock(hid_mutex);
    shutting_down = true;

    if(dev)
    {
        /* Best-effort: restore the previous onboard mode on unload. */
        unsigned char mixed_pkt[3] = { OPENRGB_CMD, SUB_SET_MIXED_MODE, 0 };
        xfer_locked(mixed_pkt, sizeof(mixed_pkt), nullptr);
        unsigned char direct_pkt[3] = { OPENRGB_CMD, SUB_SET_DIRECT_MODE, 0 };
        xfer_locked(direct_pkt, sizeof(direct_pkt), nullptr);
        hid_close(dev);
        dev = nullptr;
    }
}

std::string KeychronUltraController::GetLocation()
{
    std::lock_guard<std::mutex> lock(hid_mutex);
    return("HID: " + location);
}

bool KeychronUltraController::TakeLocationChanged()
{
    return(location_changed.exchange(false));
}

std::string KeychronUltraController::GetSerialString()
{
    std::lock_guard<std::mutex> lock(hid_mutex);

    if(dev == nullptr)
    {
        return("");
    }

    wchar_t serial[128];
    if(hid_get_serial_number_string(dev, serial, 128) != 0)
    {
        return("");
    }
    std::wstring w(serial);
    return(std::string(w.begin(), w.end()));
}

/*---------------------------------------------------------*\
| Send one command; optionally read the echoed response.    |
| Returns bytes read into resp, or -1. Mutex-guarded.       |
\*---------------------------------------------------------*/
int KeychronUltraController::xfer_locked(const unsigned char* payload, size_t len,
                                          unsigned char* resp)
{
    if(dev == nullptr)
    {
        return(-1);
    }

    unsigned char buf[KEYCHRON_ULTRA_EPSIZE + 1];
    memset(buf, 0x00, sizeof(buf));
    buf[0] = 0x00;                                   /* report id 0 (unnumbered) */
    memcpy(&buf[1], payload, len > KEYCHRON_ULTRA_EPSIZE ? KEYCHRON_ULTRA_EPSIZE : len);

    if(hid_write(dev, buf, KEYCHRON_ULTRA_EPSIZE + 1) < 0)
    {
        return(-1);
    }

    if(resp != nullptr)
    {
        return(hid_read_timeout(dev, resp, KEYCHRON_ULTRA_EPSIZE, 500));
    }
    return(0);
}

int KeychronUltraController::xfer(const unsigned char* payload, size_t len, unsigned char* resp)
{
    std::lock_guard<std::mutex> lock(hid_mutex);

    int ret = xfer_locked(payload, len, resp);
    if(ret >= 0)
    {
        return(ret);
    }

    /*-----------------------------------------------------------------------*\
    | The write failed, which on both Linux and Windows is what a handle to a  |
    | re-enumerated device looks like (the keyboard comes back on a different  |
    | hidraw node / device path after a sleep-wake cycle or a firmware-side    |
    | USB reset). Re-open it and retry once.                                   |
    \*-----------------------------------------------------------------------*/
    if(!ReconnectLocked())
    {
        return(-1);
    }

    return(xfer_locked(payload, len, resp));
}

unsigned int KeychronUltraController::GetLEDCountLocked()
{
    unsigned char pkt[2] = { OPENRGB_CMD, SUB_GET_LED_COUNT };
    unsigned char resp[KEYCHRON_ULTRA_EPSIZE] = { 0 };
    if(xfer_locked(pkt, sizeof(pkt), resp) > 0 && resp[0] == OPENRGB_CMD)
    {
        return((unsigned int)(resp[2] | (resp[3] << 8)));
    }
    return(0);
}

/*---------------------------------------------------------*\
| Re-open the keyboard after it re-enumerated. Caller must   |
| hold hid_mutex. Returns true if we now have a working      |
| handle to the same board.                                  |
\*---------------------------------------------------------*/
bool KeychronUltraController::ReconnectLocked()
{
    if(shutting_down)
    {
        return(false);
    }

    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if(now - last_reconnect < std::chrono::milliseconds(KEYCHRON_ULTRA_RECONNECT_COOLDOWN_MS))
    {
        return(false);
    }
    last_reconnect = now;

    if(dev != nullptr)
    {
        hid_close(dev);
        dev = nullptr;
    }

    hid_device_info* devs = hid_enumerate(KEYCHRON_ULTRA_VID, pid);
    bool reconnected      = false;

    for(hid_device_info* cur = devs; cur != nullptr; cur = cur->next)
    {
        if(cur->usage_page != KEYCHRON_ULTRA_RAW_USAGE_PAGE || cur->usage != KEYCHRON_ULTRA_RAW_USAGE)
        {
            continue;                               /* only the raw command interface */
        }

        hid_device* candidate = hid_open_path(cur->path);
        if(candidate == nullptr)
        {
            continue;
        }

        dev = candidate;

        /*-------------------------------------------------------------------*\
        | Same check detection uses. It rules out stock firmware and any      |
        | Ultra with a different LED count, but two boards of the SAME model  |
        | look identical here, and the firmware reports a fixed serial so     |
        | there is nothing left to tell them apart. Known and accepted: it    |
        | needs two of one model re-enumerating at once.                      |
        \*-------------------------------------------------------------------*/
        if(GetLEDCountLocked() != expected_leds)
        {
            hid_close(dev);
            dev = nullptr;
            continue;
        }

        if(location != cur->path)
        {
            location         = cur->path;
            location_changed = true;
        }

        /*-------------------------------------------------------------------*\
        | While we were writing into a dead handle the firmware's 3s watchdog  |
        | expired and handed the LEDs back to onboard lighting, so re-taking   |
        | direct mode is required - re-opening the handle alone leaves the     |
        | keyboard running its own effects. The keepalive thread is already    |
        | running and keeps re-arming from here on.                            |
        \*-------------------------------------------------------------------*/
        if(direct_active)
        {
            unsigned char pkt[3] = { OPENRGB_CMD, SUB_SET_DIRECT_MODE, 1 };
            xfer_locked(pkt, sizeof(pkt), nullptr);
        }

        reconnected = true;
        break;
    }

    hid_free_enumeration(devs);
    return(reconnected);
}

/*---------------------------------------------------------*\
| Ask the firmware which version of the 0x16 protocol it     |
| speaks. Returns 0 if the device did not answer, which the  |
| caller should treat as "assume the current version": the   |
| LED count check has already proved this is our firmware,   |
| and one dropped reply should not drop the keyboard.        |
\*---------------------------------------------------------*/
unsigned int KeychronUltraController::GetProtocolVersion()
{
    unsigned char pkt[2] = { OPENRGB_CMD, SUB_GET_PROTOCOL_VERSION };
    unsigned char resp[KEYCHRON_ULTRA_EPSIZE] = { 0 };

    if(xfer(pkt, sizeof(pkt), resp) > 0 && resp[0] == OPENRGB_CMD)
    {
        return((unsigned int)resp[2]);
    }
    return(0);
}

unsigned int KeychronUltraController::GetLEDCount()
{
    unsigned char pkt[2] = { OPENRGB_CMD, SUB_GET_LED_COUNT };
    unsigned char resp[KEYCHRON_ULTRA_EPSIZE] = { 0 };
    if(xfer(pkt, sizeof(pkt), resp) > 0 && resp[0] == OPENRGB_CMD)
    {
        return((unsigned int)(resp[2] | (resp[3] << 8)));
    }
    return(0);
}

bool KeychronUltraController::SetMixedMode(bool enable)
{
    if(enable)
    {
        SetDirectMode(false);
    }

    unsigned char pkt[3] = { OPENRGB_CMD, SUB_SET_MIXED_MODE,
                             (unsigned char)(enable ? 1 : 0) };
    unsigned char resp[KEYCHRON_ULTRA_EPSIZE] = { 0 };
    return(xfer(pkt, sizeof(pkt), resp) > 0 && resp[0] == OPENRGB_CMD);
}

bool KeychronUltraController::GetMixedInfo(unsigned char& layers,
                                            unsigned char& effects_per_layer)
{
    unsigned char pkt[2] = { OPENRGB_CMD, SUB_MIXED_GET_INFO };
    unsigned char resp[KEYCHRON_ULTRA_EPSIZE] = { 0 };
    if(xfer(pkt, sizeof(pkt), resp) <= 0 || resp[0] != OPENRGB_CMD || resp[2] != 0)
        return(false);
    layers = resp[3];
    effects_per_layer = resp[4];
    return(true);
}

bool KeychronUltraController::GetMixedRegions(unsigned int start, unsigned int count,
                                               std::vector<unsigned char>& masks)
{
    if(count == 0 || count > 28 || start + count > expected_leds)
        return(false);
    unsigned char pkt[4] = { OPENRGB_CMD, SUB_MIXED_GET_REGIONS,
                             (unsigned char)start, (unsigned char)count };
    unsigned char resp[KEYCHRON_ULTRA_EPSIZE] = { 0 };
    if(xfer(pkt, sizeof(pkt), resp) <= 0 || resp[0] != OPENRGB_CMD || resp[2] != 0)
        return(false);
    masks.assign(resp + 3, resp + 3 + count);
    return(true);
}

bool KeychronUltraController::SetMixedRegions(unsigned int start,
                                               const std::vector<unsigned char>& masks)
{
    if(masks.empty() || masks.size() > 28 || start + masks.size() > expected_leds)
        return(false);
    unsigned char pkt[KEYCHRON_ULTRA_EPSIZE];
    pkt[0] = OPENRGB_CMD;
    pkt[1] = SUB_MIXED_SET_REGIONS;
    pkt[2] = (unsigned char)start;
    pkt[3] = (unsigned char)masks.size();
    for(size_t i = 0; i < masks.size(); i++)
        pkt[4 + i] = masks[i];
    unsigned char resp[KEYCHRON_ULTRA_EPSIZE] = { 0 };
    return(xfer(pkt, 4 + masks.size(), resp) > 0 && resp[0] == OPENRGB_CMD
           && resp[2] == 0);
}

bool KeychronUltraController::GetMixedEffect(unsigned char layer, unsigned char slot,
                                              KeychronMixedEffect& effect)
{
    if(layer >= 3 || slot >= 3)
        return(false);
    unsigned char pkt[5] = { OPENRGB_CMD, SUB_MIXED_GET_EFFECTS, layer, slot, 1 };
    unsigned char resp[KEYCHRON_ULTRA_EPSIZE] = { 0 };
    if(xfer(pkt, sizeof(pkt), resp) <= 0 || resp[0] != OPENRGB_CMD || resp[2] != 0)
        return(false);
    effect.effect = resp[3];
    effect.hue = resp[4];
    effect.saturation = resp[5];
    effect.speed = resp[6];
    effect.value = resp[7];
    effect.time = (unsigned int)resp[8] | ((unsigned int)resp[9] << 8)
                | ((unsigned int)resp[10] << 16) | ((unsigned int)resp[11] << 24);
    return(true);
}

bool KeychronUltraController::SetMixedEffect(unsigned char layer, unsigned char slot,
                                              const KeychronMixedEffect& effect)
{
    if(layer >= 3 || slot >= 3)
        return(false);
    unsigned char pkt[14] = { OPENRGB_CMD, SUB_MIXED_SET_EFFECTS, layer, slot, 1,
                              effect.effect, effect.hue, effect.saturation, effect.speed,
                              effect.value,
                              (unsigned char)(effect.time & 0xFF),
                              (unsigned char)((effect.time >> 8) & 0xFF),
                              (unsigned char)((effect.time >> 16) & 0xFF),
                              (unsigned char)((effect.time >> 24) & 0xFF) };
    unsigned char resp[KEYCHRON_ULTRA_EPSIZE] = { 0 };
    return(xfer(pkt, sizeof(pkt), resp) > 0 && resp[0] == OPENRGB_CMD && resp[2] == 0);
}

bool KeychronUltraController::SaveMixedSettings()
{
    unsigned char pkt[2] = { OPENRGB_CMD, SUB_MIXED_SAVE };
    unsigned char resp[KEYCHRON_ULTRA_EPSIZE] = { 0 };
    return(xfer(pkt, sizeof(pkt), resp) > 0 && resp[0] == OPENRGB_CMD && resp[2] == 0);
}

bool KeychronUltraController::SetMasterBrightness(unsigned char brightness)
{
    unsigned char pkt[3] = { OPENRGB_CMD, SUB_SET_MASTER_BRIGHTNESS, brightness };
    unsigned char resp[KEYCHRON_ULTRA_EPSIZE] = { 0 };
    return(xfer(pkt, sizeof(pkt), resp) > 0 && resp[0] == OPENRGB_CMD);
}

void KeychronUltraController::SetDirectMode(bool enable)
{
    /*-----------------------------------------------------------------------*\
    | Always stop AND join any running keepalive before changing state, so we |
    | never reassign a still-joinable std::thread (that calls std::terminate  |
    | and crashes the host). SetDirectMode is only called on mode changes, so |
    | the join cost is irrelevant.                                            |
    \*-----------------------------------------------------------------------*/
    std::lock_guard<std::mutex> state_lock(state_mutex);

    if(keepalive_run.exchange(false) && keepalive_thread.joinable())
    {
        keepalive_thread.join();
    }

    unsigned char pkt[3] = { OPENRGB_CMD, SUB_SET_DIRECT_MODE, (unsigned char)(enable ? 1 : 0) };
    xfer(pkt, sizeof(pkt), nullptr);
    direct_active = enable;

    if(enable)
    {
        keepalive_run    = true;
        keepalive_thread = std::thread(&KeychronUltraController::KeepaliveLoop, this);
    }
}

void KeychronUltraController::EnsureDirect()
{
    if(!direct_active)
    {
        SetDirectMode(true);
    }
}

/*---------------------------------------------------------*\
| Re-arm the firmware's 3s auto-hand-back watchdog while     |
| OpenRGB holds the device, so colors persist between edits. |
\*---------------------------------------------------------*/
void KeychronUltraController::KeepaliveLoop()
{
    while(keepalive_run)
    {
        for(int i = 0; i < KEEPALIVE_INTERVAL_MS / 50 && keepalive_run; i++)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if(keepalive_run)
        {
            unsigned char pkt[3] = { OPENRGB_CMD, SUB_SET_DIRECT_MODE, 1 };
            xfer(pkt, sizeof(pkt), nullptr);
        }
    }
}

/*---------------------------------------------------------*\
| Stream all LED colors in runs of <=9 (fits one 32-byte    |
| packet: 4 header bytes + 9*3 RGB).                        |
\*---------------------------------------------------------*/
void KeychronUltraController::SetLEDs(const std::vector<RGBColor>& colors)
{
    unsigned int total = (unsigned int)colors.size();
    unsigned int i     = 0;

    /*-----------------------------------------------------------------------*\
    | The start index goes on the wire as one byte, so anything past 256 LEDs  |
    | cannot be addressed. Stop there instead of letting the index wrap and    |
    | write colours over the start of the board.                               |
    \*-----------------------------------------------------------------------*/
    if(total > KEYCHRON_ULTRA_MAX_LEDS)
    {
        total = KEYCHRON_ULTRA_MAX_LEDS;
    }

    /*-----------------------------------------------------------------------*\
    | Remember the frame so RestoreLastFrame() can put it back if a rescan     |
    | throws this controller away and builds a new one.                        |
    \*-----------------------------------------------------------------------*/
    {
        std::lock_guard<std::mutex> frame_lock(frame_mutex);
        last_frame[pid] = colors;
    }

    while(i < total)
    {
        unsigned int cnt = (total - i) < KEYCHRON_ULTRA_LEDS_PER_PKT
                         ? (total - i) : KEYCHRON_ULTRA_LEDS_PER_PKT;

        unsigned char pkt[4 + KEYCHRON_ULTRA_LEDS_PER_PKT * 3];
        pkt[0] = OPENRGB_CMD;
        pkt[1] = SUB_SET_LEDS;
        pkt[2] = (unsigned char)i;                   /* start index */
        pkt[3] = (unsigned char)cnt;                 /* run length  */

        for(unsigned int j = 0; j < cnt; j++)
        {
            RGBColor c         = colors[i + j];
            pkt[4 + j * 3 + 0] = RGBGetRValue(c);
            pkt[4 + j * 3 + 1] = RGBGetGValue(c);
            pkt[4 + j * 3 + 2] = RGBGetBValue(c);
        }

        xfer(pkt, 4 + cnt * 3, nullptr);
        i += cnt;
    }
}

void KeychronUltraController::RestoreLastFrame()
{
    std::vector<RGBColor> frame;

    {
        std::lock_guard<std::mutex> frame_lock(frame_mutex);
        std::map<unsigned short, std::vector<RGBColor> >::iterator it = last_frame.find(pid);
        if(it == last_frame.end())
        {
            return;              /* first time we have seen this board this run */
        }
        frame = it->second;
    }

    if(frame.size() != expected_leds)
    {
        return;                  /* different board behind the same PID */
    }

    /*-----------------------------------------------------------------------*\
    | The old controller handed the board back to its onboard lighting on the  |
    | way out, so take it again and repaint. Without this the keyboard sits on |
    | its own effects after a rescan until something happens to send a frame.  |
    \*-----------------------------------------------------------------------*/
    SetDirectMode(true);
    SetLEDs(frame);
}

bool KeychronUltraController::GetLEDPosition(unsigned int idx, unsigned int& x,
                                               unsigned int& y, unsigned int& flags)
{
    if(idx >= KEYCHRON_ULTRA_MAX_LEDS)
    {
        return(false);           /* the index is one byte on the wire */
    }

    unsigned char pkt[3]  = { OPENRGB_CMD, SUB_GET_LED_INFO, (unsigned char)idx };
    unsigned char resp[KEYCHRON_ULTRA_EPSIZE] = { 0 };
    if(xfer(pkt, sizeof(pkt), resp) > 0 && resp[0] == OPENRGB_CMD)
    {
        x     = resp[3];   /* args[1] = x (resp[0]=cmd,[1]=sub,[2]=idx echo,[3]=x) */
        y     = resp[4];   /* args[2] = y */
        flags = resp[5];   /* args[3] = flags */
        return(true);
    }
    return(false);
}
