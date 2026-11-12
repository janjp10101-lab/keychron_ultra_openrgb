/*---------------------------------------------------------*\
| OpenRGBKeychronUltraPlugin.cpp                          |
\*---------------------------------------------------------*/

#include "OpenRGBKeychronUltraPlugin.h"
#include "KeychronUltraController.h"
#include "KeychronLayouts.h"
#include <algorithm>
#include <chrono>
#include <thread>
#include <hidapi.h>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QCheckBox>
#include <QSlider>
#include <QPushButton>
#include <QColorDialog>
#include <QColor>
#include <QFormLayout>
#include <memory>


/*---------------------------------------------------------------------------*\
| VID and the raw-interface usage filter come from KeychronUltraController.h  |
| so that detection here and the controller's reconnect path can never drift    |
| apart and match different interfaces.                                        |
\*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*\
| The set of supported boards (PID + display name + LED layout) lives in       |
| KEYCHRON_LAYOUTS[] (KeychronLayouts.h). Every board speaks the same 0x16     |
| raw-HID protocol; only the PID, name and per-key layout differ.              |
\*---------------------------------------------------------------------------*/

OpenRGBPluginInfo OpenRGBKeychronUltraPlugin::GetPluginInfo()
{
    OpenRGBPluginInfo info;
    info.Name          = "Keychron Ultra (Mixed RGB)";
    info.Description    = "On-device Mixed RGB layer controls for Keychron V- and Q-series Ultra keyboards.";
    info.Version        = "0.6.0";
    info.Commit         = "";
    info.URL            = "https://github.com/janjp10101-lab/keychron_ultra_openrgb";
    info.Location       = OPENRGB_PLUGIN_LOCATION_SETTINGS;
    info.Label          = "Keychron Ultra";
    info.TabIconString  = "";
    return(info);
}

unsigned int OpenRGBKeychronUltraPlugin::GetPluginAPIVersion()
{
    return(OPENRGB_PLUGIN_API_VERSION);
}

void OpenRGBKeychronUltraPlugin::Load(ResourceManagerInterface* resource_manager)
{
    rm = resource_manager;

    /*-----------------------------------------------------------------------*\
    | Safe to call again, hidapi ignores a second init. There is deliberately  |
    | no matching hid_exit() in Unload(): hidapi state is global and OpenRGB   |
    | owns it, so tearing it down from a plugin would pull it out from under   |
    | the host and every other device it has open.                             |
    \*-----------------------------------------------------------------------*/
    hid_init();

    DetectControllers();

    /*-----------------------------------------------------------------------*\
    | OpenRGB only loads a plugin once per run. Its onDetectionEnded() keeps  |
    | a plugins_loaded flag and skips loading after the first time. But every |
    | rescan calls ResourceManager::Cleanup(), and that deletes everything in |
    | rgb_controllers_hw, which is the same list RegisterRGBController() puts |
    | our devices in.                                                         |
    |                                                                         |
    | So a rescan destroys our keyboard and nobody asks us to make a new one. |
    | It disappears for the rest of the session, and the pointers we kept are |
    | now dangling, ready to be freed a second time in Unload().              |
    |                                                                         |
    | Rebuilding at the end of detection fixes both. Cleanup() has already    |
    | run by then, the device list is finished, and it fires on the first     |
    | detection as well as on every rescan.                                   |
    \*-----------------------------------------------------------------------*/
    rm->RegisterDetectionEndCallback(&OpenRGBKeychronUltraPlugin::OnDetectionEnd, this);
}

void OpenRGBKeychronUltraPlugin::OnDetectionEnd(void* arg)
{
    OpenRGBKeychronUltraPlugin* plugin = (OpenRGBKeychronUltraPlugin*)arg;

    plugin->DropDeletedControllers();
    plugin->DetectControllers();
}

/*---------------------------------------------------------------------------*\
| Drop the controllers OpenRGB has already destroyed. Do not delete them here, |
| Cleanup() owns them and has freed them already. Keeping a stale one would    |
| hand it to UnregisterRGBController(), which reads its name for the log and   |
| calls a virtual on it, and then we would free it a second time.              |
|                                                                              |
| We decide what is still alive by comparing pointers against the resource     |
| manager's own list, which Cleanup() removes ours from. Comparing a pointer   |
| is safe, so nothing freed is ever read.                                      |
\*---------------------------------------------------------------------------*/
void OpenRGBKeychronUltraPlugin::DropDeletedControllers()
{
    if(rm == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(registered_mutex);

    const std::vector<RGBController*>&          live = rm->GetRGBControllers();
    std::vector<RGBController_KeychronUltra*> alive;

    for(RGBController_KeychronUltra* rgb : registered)
    {
        if(std::find(live.begin(), live.end(), (RGBController*)rgb) != live.end())
        {
            alive.push_back(rgb);
        }
    }

    registered = alive;
}

void OpenRGBKeychronUltraPlugin::DetectControllers()
{
    std::lock_guard<std::mutex> lock(registered_mutex);

    /*-----------------------------------------------------------------------*\
    | Our devices came through the detection cycle alive, which happens when  |
    | Cleanup() did not run at all. Registering again here would show the     |
    | keyboard twice and open a second handle to it.                          |
    \*-----------------------------------------------------------------------*/
    if(!registered.empty())
    {
        return;
    }

    for(unsigned int i = 0; i < KEYCHRON_LAYOUT_COUNT; i++)
    {
        const KeychronLayout* layout = &KEYCHRON_LAYOUTS[i];

        hid_device_info* devs = hid_enumerate(KEYCHRON_ULTRA_VID, layout->pid);
        for(hid_device_info* cur = devs; cur != nullptr; cur = cur->next)
        {
            if(cur->usage_page != KEYCHRON_ULTRA_RAW_USAGE_PAGE
            || cur->usage      != KEYCHRON_ULTRA_RAW_USAGE)
            {
                continue;                               /* only the raw command interface */
            }

            hid_device* dev = hid_open_path(cur->path);
            if(dev == nullptr)
            {
                continue;
            }

            KeychronUltraController* ctrl = new KeychronUltraController(dev, cur->path,
                                                                           layout->pid,
                                                                           layout->led_count);

            /*---------------------------------------------------------------*\
            | Stock firmware does not answer this at all and returns 0, so one |
            | call rules out both stock firmware and a PID whose layout says a |
            | different number of LEDs.                                        |
            \*---------------------------------------------------------------*/
            if(ctrl->GetLEDCount() != layout->led_count)
            {
                delete ctrl;
                continue;
            }

            /*---------------------------------------------------------------*\
            | Turn away firmware newer than we know how to talk to. A device   |
            | that does not answer reports 0, and we let that through, because |
            | the LED count above already proved it is our firmware and one    |
            | lost reply should not cost the user their keyboard.              |
            \*---------------------------------------------------------------*/
            unsigned int proto = ctrl->GetProtocolVersion();
            if(proto > KEYCHRON_ULTRA_PROTOCOL_VERSION)
            {
                delete ctrl;
                continue;
            }

            RGBController_KeychronUltra* rgb = new RGBController_KeychronUltra(ctrl, layout);
            rm->RegisterRGBController(rgb);
            registered.push_back(rgb);

            /*---------------------------------------------------------------*\
            | If a rescan just destroyed the previous controller, put the      |
            | colours back. Does nothing on the first detection of the run.    |
            \*---------------------------------------------------------------*/
            ctrl->RestoreLastFrame();
        }
        hid_free_enumeration(devs);
    }
}

bool OpenRGBKeychronUltraPlugin::ApplyMixedLayer(unsigned char layer, bool enabled,
                                                       unsigned char hue,
                                                       unsigned char saturation,
                                                       unsigned char speed,
                                                       unsigned char brightness)
{
    std::lock_guard<std::mutex> lock(registered_mutex);
    return(!registered.empty() &&
           registered.front()->ConfigureMixedLayer(layer, enabled, hue, saturation, speed, brightness));
}

bool OpenRGBKeychronUltraPlugin::ApplyMixedBrightness(unsigned char brightness)
{
    std::lock_guard<std::mutex> lock(registered_mutex);
    return(!registered.empty() && registered.front()->SetMixedMasterBrightness(brightness));
}

bool OpenRGBKeychronUltraPlugin::SaveMixedSettings()
{
    std::lock_guard<std::mutex> lock(registered_mutex);
    return(!registered.empty() && registered.front()->SaveMixedConfiguration());
}

QWidget* OpenRGBKeychronUltraPlugin::GetWidget()
{
    QWidget* root = new QWidget();
    QVBoxLayout* main_layout = new QVBoxLayout(root);

    bool has_device = false;
    {
        std::lock_guard<std::mutex> lock(registered_mutex);
        has_device = !registered.empty();
    }

    QLabel* intro = new QLabel(
        "Keychron Ultra Mixed RGB\n"
        "The keyboard renders these effects locally. OpenRGB only sends settings.");
    intro->setWordWrap(true);
    main_layout->addWidget(intro);

    if(!has_device)
    {
        QLabel* missing = new QLabel("Connect your custom-firmware Keychron Ultra keyboard, then rescan devices.");
        missing->setWordWrap(true);
        main_layout->addWidget(missing);
        main_layout->addStretch();
        return(root);
    }

    struct LayerUI
    {
        bool enabled;
        unsigned char hue;
        unsigned char saturation;
        unsigned char speed;
        unsigned char brightness;
        const char* name;
        const char* color;
    };
    LayerUI layers[3] = {
        { true, 230, 255, 127, 255, "Static keys", "#ff00aa" },
        { true,  88, 255, 127, 255, "Spiral",      "#aaff00" },
        { true, 190, 255, 127, 255, "Keypress splash", "#aa00ff" }
    };

    for(int i = 0; i < 3; i++)
    {
        const int layer = i;
        QGroupBox* group = new QGroupBox(layers[i].name);
        QVBoxLayout* group_layout = new QVBoxLayout(group);
        QHBoxLayout* controls = new QHBoxLayout();

        QCheckBox* enabled = new QCheckBox("Enabled");
        enabled->setChecked(layers[i].enabled);

        QPushButton* color_button = new QPushButton("Choose color");
        std::shared_ptr<QColor> current_color = std::make_shared<QColor>(layers[i].color);
        color_button->setStyleSheet(QString("background-color: %1;").arg(current_color->name()));

        QSlider* speed = new QSlider(Qt::Horizontal);
        speed->setRange(0, 255);
        speed->setValue(layers[i].speed);
        QLabel* speed_label = new QLabel(QString("Speed %1").arg(layers[i].speed));

        QSlider* layer_brightness = new QSlider(Qt::Horizontal);
        layer_brightness->setRange(0, 255);
        layer_brightness->setValue(layers[i].brightness);
        QLabel* layer_brightness_label = new QLabel(
            QString("Layer brightness %1").arg(layers[i].brightness));

        controls->addWidget(enabled);
        controls->addWidget(color_button);
        group_layout->addLayout(controls);
        group_layout->addWidget(speed_label);
        group_layout->addWidget(speed);
        group_layout->addWidget(layer_brightness_label);
        group_layout->addWidget(layer_brightness);
        main_layout->addWidget(group);

        auto apply = [this, layer, enabled, speed, layer_brightness, current_color]() {
            int hue = current_color->hsvHue();
            int sat = current_color->hsvSaturation();
            if(hue < 0) hue = 0;
            ApplyMixedLayer((unsigned char)layer, enabled->isChecked(),
                (unsigned char)(hue * 255 / 359), (unsigned char)sat,
                (unsigned char)speed->value(), (unsigned char)layer_brightness->value());
        };

        QObject::connect(enabled, &QCheckBox::toggled, root,
            [apply](bool) { apply(); });
        QObject::connect(speed, &QSlider::valueChanged, root,
            [apply, speed_label](int value) {
                speed_label->setText(QString("Speed %1").arg(value));
                apply();
            });
        QObject::connect(layer_brightness, &QSlider::valueChanged, root,
            [apply, layer_brightness_label](int value) {
                layer_brightness_label->setText(QString("Layer brightness %1").arg(value));
                apply();
            });
        QObject::connect(color_button, &QPushButton::clicked, root,
            [root, color_button, current_color, apply]() {
                QColor chosen = QColorDialog::getColor(*current_color, root, "Layer color");
                if(!chosen.isValid()) return;
                *current_color = chosen;
                color_button->setStyleSheet(QString("background-color: %1;").arg(chosen.name()));
                apply();
            });
    }

    QGroupBox* brightness_group = new QGroupBox("Master brightness");
    QVBoxLayout* brightness_layout = new QVBoxLayout(brightness_group);
    QSlider* brightness = new QSlider(Qt::Horizontal);
    brightness->setRange(0, 255);
    brightness->setValue(255);
    QLabel* brightness_label = new QLabel("Brightness 255");
    brightness_layout->addWidget(brightness_label);
    brightness_layout->addWidget(brightness);
    main_layout->addWidget(brightness_group);

    QObject::connect(brightness, &QSlider::valueChanged, root,
        [this, brightness_label](int value) {
            brightness_label->setText(QString("Brightness %1").arg(value));
            ApplyMixedBrightness((unsigned char)value);
        });

    QPushButton* save = new QPushButton("Save to keyboard");
    QObject::connect(save, &QPushButton::clicked, root,
        [this]() { SaveMixedSettings(); });
    main_layout->addWidget(save);
    main_layout->addStretch();
    return(root);
}

QMenu* OpenRGBKeychronUltraPlugin::GetTrayMenu()
{
    return(nullptr);
}

void OpenRGBKeychronUltraPlugin::Unload()
{
    /*-----------------------------------------------------------------------*\
    | Stop the callback first. UnloadPlugins() can dlclose this plugin, and a |
    | detection cycle afterwards would call into unmapped code.               |
    \*-----------------------------------------------------------------------*/
    if(rm != nullptr)
    {
        rm->UnregisterDetectionEndCallback(&OpenRGBKeychronUltraPlugin::OnDetectionEnd, this);
    }

    /*-----------------------------------------------------------------------*\
    | Anything a rescan already destroyed is not ours to unregister or free.  |
    | Called before taking the lock below - it takes the same one.            |
    \*-----------------------------------------------------------------------*/
    DropDeletedControllers();

    std::lock_guard<std::mutex> lock(registered_mutex);

    /*-----------------------------------------------------------------------*\
    | Take every device out of the list before freeing any of them.           |
    |                                                                         |
    | OpenRGB's SDK server keeps a reference to the same vector the resource  |
    | manager stores controllers in, and SendReply_ControllerData() calls     |
    | GetDeviceDescription() on whatever it finds there without taking any    |
    | lock. If a client such as Artemis asks for device data while we are     |
    | deleting, the server can read an object we have already freed. That is  |
    | a real crash and we have seen it, a SIGSEGV inside memcpy under         |
    | GetDeviceDescription during shutdown.                                   |
    |                                                                         |
    | Unregistering first means no new request can find these devices.        |
    \*-----------------------------------------------------------------------*/
    for(RGBController_KeychronUltra* rgb : registered)
    {
        if(rm != nullptr)
        {
            rm->UnregisterRGBController(rgb);
        }
    }

    /*-----------------------------------------------------------------------*\
    | A request that was already running when we unregistered still holds its |
    | pointer, and there is no way for a plugin to wait for it: the server    |
    | offers nothing to synchronise on. Give it a moment to finish instead.   |
    | This only narrows the race, it does not close it, and the only real fix |
    | belongs upstream. Unload runs on shutdown, so the wait costs nothing.   |
    \*-----------------------------------------------------------------------*/
    if(!registered.empty())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    for(RGBController_KeychronUltra* rgb : registered)
    {
        delete rgb;                                     /* also closes the HID handle */
    }
    registered.clear();
}
