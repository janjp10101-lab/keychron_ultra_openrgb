/*---------------------------------------------------------*\
| RGBController_KeychronUltra.h                           |
|                                                           |
|   OpenRGB RGBController wrapper for the Keychron V6 Ultra  |
|   8K custom-firmware OpenRGB direct-control support.       |
\*---------------------------------------------------------*/

#pragma once

#include <mutex>

#include "RGBController.h"
#include "KeychronUltraController.h"
#include "KeychronLayouts.h"

class RGBController_KeychronUltra : public RGBController
{
public:
    RGBController_KeychronUltra(KeychronUltraController* controller_ptr,
                                  const KeychronLayout* layout_ptr);
    ~RGBController_KeychronUltra();

    void        SetupZones();
    void        ResizeZone(int zone, int new_size);

    void        DeviceUpdateLEDs();
    void        UpdateZoneLEDs(int zone);
    void        UpdateSingleLED(int led);

    void        DeviceUpdateMode();
    void        SetCustomMode();
    bool        ConfigureMixedLayer(unsigned char layer, bool enabled,
                                    unsigned char hue, unsigned char saturation,
                                    unsigned char speed, unsigned char brightness);
    bool        SaveMixedConfiguration();
    bool        GetMixedLayerConfig(unsigned char layer, KeychronMixedEffect& effect,
                                    bool& enabled);
    bool        GetMixedMasterBrightness(unsigned char& brightness);
    bool        SetMixedMasterBrightness(unsigned char brightness);

private:
    void        UpdateLEDsLocked();         /* caller must hold controller_mutex */

    KeychronUltraController* controller;
    const KeychronLayout*      layout;
    matrix_map_type            matrix;      /* backs the zone's matrix_map pointer */

    /*-----------------------------------------------------------------------*\
    | Guards `controller` against the base class's DeviceCallThread - see the |
    | destructor for why that thread can outlive the pointer.                 |
    \*-----------------------------------------------------------------------*/
    std::mutex                 controller_mutex;
};
