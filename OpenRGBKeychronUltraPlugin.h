/*---------------------------------------------------------*\
| OpenRGBKeychronUltraPlugin.h                            |
|                                                           |
|   OpenRGB plugin that registers the Keychron V6 Ultra 8K  |
|   (custom ZMK firmware, OpenRGB direct control) as an      |
|   OpenRGB device. API v4 (OpenRGB 1.0rc3).                |
\*---------------------------------------------------------*/

#pragma once

#include <QObject>
#include <QWidget>
#include <QMenu>
#include <mutex>
#include <vector>

#include "OpenRGBPluginInterface.h"
#include "ResourceManagerInterface.h"
#include "RGBController_KeychronUltra.h"

class OpenRGBKeychronUltraPlugin : public QObject, public OpenRGBPluginInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID OpenRGBPluginInterface_IID)
    Q_INTERFACES(OpenRGBPluginInterface)

public:
    OpenRGBPluginInfo   GetPluginInfo()                                    override;
    unsigned int        GetPluginAPIVersion()                             override;
    void                Load(ResourceManagerInterface* resource_manager)  override;
    QWidget*            GetWidget()                                        override;
    QMenu*              GetTrayMenu()                                      override;
    void                Unload()                                          override;

private:
    /*-----------------------------------------------------------------------*\
    | Fired by ResourceManager::ProcessPostDetection at the end of every      |
    | detection cycle, including every "Rescan Devices". See Load() for why   |
    | the plugin has to re-detect there.                                     |
    \*-----------------------------------------------------------------------*/
    static void         OnDetectionEnd(void* arg);

    void                DetectControllers();
    void                DropDeletedControllers();
    bool                ApplyMixedLayer(unsigned char layer, bool enabled,
                                        unsigned char hue, unsigned char saturation,
                                        unsigned char speed);
    bool                ApplyMixedBrightness(unsigned char brightness);
    bool                SaveMixedSettings();

    ResourceManagerInterface*                    rm = nullptr;
    std::vector<RGBController_KeychronUltra*>  registered;

    /*-----------------------------------------------------------------------*\
    | registered is touched from the detection thread (OnDetectionEnd) and     |
    | from the GUI thread (Unload), so it needs guarding.                     |
    \*-----------------------------------------------------------------------*/
    std::mutex                                   registered_mutex;
};
