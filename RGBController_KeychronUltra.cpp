/*---------------------------------------------------------*\
| RGBController_KeychronUltra.cpp                         |
|                                                           |
|   Generic OpenRGB wrapper for any Keychron Ultra board    |
|   running the custom ZMK firmware. All board-specific     |
|   data (LED count, physical matrix map, per-key names)    |
|   comes from the KeychronLayout descriptor selected by    |
|   USB PID in the plugin. See KeychronLayouts.h.            |
\*---------------------------------------------------------*/

#include "RGBController_KeychronUltra.h"
#include <algorithm>


/*---------------------------------------------------------*\
| Modes                                                      |
\*---------------------------------------------------------*/
enum
{
    MODE_DIRECT = 0,
    MODE_MIXED_RGB = 1,
};

RGBController_KeychronUltra::RGBController_KeychronUltra(KeychronUltraController* controller_ptr,
                                                            const KeychronLayout* layout_ptr)
{
    controller  = controller_ptr;
    layout      = layout_ptr;

    name        = layout->name;
    vendor      = "Keychron";
    type        = DEVICE_TYPE_KEYBOARD;
    description = layout->description;
    location    = controller->GetLocation();
    serial      = controller->GetSerialString();

    mode Direct;
    Direct.name       = "Direct";
    Direct.value      = MODE_DIRECT;
    Direct.flags      = MODE_FLAG_HAS_PER_LED_COLOR;
    Direct.color_mode = MODE_COLORS_PER_LED;
    modes.push_back(Direct);

    mode Mixed;
    Mixed.name       = "Mixed RGB";
    Mixed.value      = MODE_MIXED_RGB;
    Mixed.flags      = 0;
    Mixed.color_mode = MODE_COLORS_PER_LED;
    modes.push_back(Mixed);

    SetupZones();

    active_mode = MODE_DIRECT;
}

RGBController_KeychronUltra::~RGBController_KeychronUltra()
{
    /*-----------------------------------------------------------------------*\
    | The base class starts a thread that calls DeviceUpdateLEDs() and        |
    | DeviceUpdateMode() over and over, and only the base destructor stops    |
    | it. Base destructors run after this one, so the thread is still going   |
    | while we are in here. Deleting the controller without the lock would be |
    | a use after free. This is not a rare shutdown path either, because      |
    | OpenRGB destroys plugin controllers on every rescan.                    |
    |                                                                         |
    | Taking the same lock the update methods take means an update in flight  |
    | either finishes first or sees a null pointer and does nothing.          |
    |                                                                         |
    | It also settles a second race. The controller destructor stops the      |
    | keepalive thread without holding its own state lock, but the only       |
    | caller of SetDirectMode() is DeviceUpdateMode(), which cannot run while |
    | we hold this.                                                           |
    |                                                                         |
    | One gap is left and a plugin cannot close it. Once the base destructor  |
    | starts, the vtable pointer drops back to the base class while its       |
    | thread is still running, so a call queued in that moment lands on a     |
    | pure virtual. The thread and its flags are private in RGBController, so |
    | there is no way to stop it any earlier from here.                       |
    \*-----------------------------------------------------------------------*/
    std::lock_guard<std::mutex> lock(controller_mutex);

    delete controller;
    controller = nullptr;
}

void RGBController_KeychronUltra::SetupZones()
{
    /*-----------------------------------------------------------------------*\
    | The zone keeps a pointer to matrix, so matrix has to outlive this call.  |
    | That is why it is a member. Its map, height and width come straight     |
    | from the layout.                                                        |
    \*-----------------------------------------------------------------------*/
    matrix.height = layout->map_height;
    matrix.width  = layout->map_width;
    matrix.map    = layout->matrix_map;

    zone kb_zone;
    kb_zone.name       = "Keyboard";
    kb_zone.type       = ZONE_TYPE_MATRIX;
    kb_zone.leds_min   = layout->led_count;
    kb_zone.leds_max   = layout->led_count;
    kb_zone.leds_count = layout->led_count;
    kb_zone.matrix_map = &matrix;
    zones.push_back(kb_zone);

    for(unsigned int i = 0; i < layout->led_count; i++)
    {
        led new_led;
        new_led.name  = layout->led_names[i];
        new_led.value = i;
        leds.push_back(new_led);
    }

    SetupColors();
}

void RGBController_KeychronUltra::ResizeZone(int /*zone*/, int /*new_size*/)
{
    /* the layout is fixed per board, so there is nothing to resize */
}

void RGBController_KeychronUltra::DeviceUpdateLEDs()
{
    std::lock_guard<std::mutex> lock(controller_mutex);

    UpdateLEDsLocked();
}

void RGBController_KeychronUltra::UpdateLEDsLocked()
{
    if(controller == nullptr)
    {
        return;                   /* torn down by a rescan - nothing to drive */
    }

    if(active_mode == MODE_DIRECT)
    {
        controller->EnsureDirect();
        controller->SetLEDs(colors);
    }

    /*-----------------------------------------------------------------------*\
    | A reconnect can move the keyboard to a different HID node, so refresh   |
    | the location we cached when this object was built. Without this OpenRGB |
    | and anything reading it, such as Artemis, keep showing a node that no   |
    | longer exists, which is hard to make sense of when something breaks.    |
    | The check itself is one atomic read when nothing has changed.           |
    \*-----------------------------------------------------------------------*/
    if(controller->TakeLocationChanged())
    {
        location = controller->GetLocation();
    }
}

void RGBController_KeychronUltra::UpdateZoneLEDs(int /*zone*/)
{
    DeviceUpdateLEDs();
}

void RGBController_KeychronUltra::UpdateSingleLED(int /*led*/)
{
    DeviceUpdateLEDs();
}

void RGBController_KeychronUltra::DeviceUpdateMode()
{
    std::lock_guard<std::mutex> lock(controller_mutex);

    if(controller == nullptr)
    {
        return;
    }

    /*-----------------------------------------------------------------------*\
    | Direct is the only mode the host drives. Entering it takes the keyboard |
    | over and starts the keepalive. Leaving it gives the board back to its   |
    | own lighting.                                                           |
    \*-----------------------------------------------------------------------*/
    if(active_mode == MODE_DIRECT)
    {
        controller->SetMixedMode(false);
        controller->SetDirectMode(true);
        UpdateLEDsLocked();
    }
    else if(active_mode == MODE_MIXED_RGB)
    {
        controller->SetDirectMode(false);
        controller->SetMixedMode(true);
    }
    else
    {
        controller->SetMixedMode(false);
        controller->SetDirectMode(false);
    }
}

void RGBController_KeychronUltra::SetCustomMode()
{
    active_mode = MODE_DIRECT;
}

bool RGBController_KeychronUltra::ConfigureMixedLayer(unsigned char layer, bool enabled,
                                                        unsigned char hue,
                                                        unsigned char saturation,
                                                        unsigned char speed,
                                                        unsigned char brightness)
{
    if(layer >= 3)
        return(false);

    std::lock_guard<std::mutex> lock(controller_mutex);
    if(controller == nullptr || !controller->SetMixedMode(true))
        return(false);
    active_mode = MODE_MIXED_RGB;

    KeychronMixedEffect effect;
    if(!controller->GetMixedEffect(layer, 0, effect))
        return(false);
    static const unsigned char effect_ids[3] = { 23, 3, 22 };
    effect.effect = enabled ? effect_ids[layer] : 0;
    effect.hue = hue;
    effect.saturation = saturation;
    effect.speed = speed;
    effect.value = brightness;
    return(controller->SetMixedEffect(layer, 0, effect));
}

bool RGBController_KeychronUltra::SaveMixedConfiguration()
{
    std::lock_guard<std::mutex> lock(controller_mutex);
    return(controller != nullptr && controller->SaveMixedSettings());
}

bool RGBController_KeychronUltra::SetMixedMasterBrightness(unsigned char brightness)
{
    std::lock_guard<std::mutex> lock(controller_mutex);
    if(controller == nullptr || !controller->SetMixedMode(true))
        return(false);
    active_mode = MODE_MIXED_RGB;
    return(controller->SetMasterBrightness(brightness));
}
