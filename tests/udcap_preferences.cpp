#include "udcap_preferences.hpp"
#include "desktop_preferences.hpp"

#include <windows.h>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace {
using namespace opencap;

void put(const std::filesystem::path& path, std::string_view content) {
    auto out = std::ofstream(path, std::ios::binary);
    out << content;
    if (!out) throw std::runtime_error("Could not write preference fixture");
}

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

bool closeTo(double a, double b) { return std::abs(a - b) < 1e-5; }

struct Fixture {
    const std::filesystem::path root = std::filesystem::temp_directory_path()
        / (L"OpenCap-UdcapChecks-" + std::to_wstring(GetCurrentProcessId()));
    UdcapSources sources{root / L"factory.config", root / L"config.json", root / L"default.vrsettings", root / L"openvrpaths.vrpath"};
    const std::filesystem::path vr = root / L"steamvr.vrsettings";
    Fixture() { std::filesystem::create_directory(root); }
    ~Fixture() {
        std::error_code error;
        for (const auto& path : {sources.desktopDefaults, sources.desktopSettings, sources.steamvrDefaults,
                                 sources.steamvrRegistry, vr, root / L"settings.txt", root / L"steamvr-switch.json", root})
            std::filesystem::remove(path, error);
    }
};
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 2) {
            const auto data = readUdcapPreferences(findUdcapSources(argv[1]));
            require(data.found && data.saved, "Installed saved UDCAP settings are readable");
            for (const auto& hand : data.receiver)
                require(hand.controller.size() == 13 && hand.hasCurves, "Installed glove preferences map both hands");
            std::cout << "Installed settings mapped for both hands; saved tracker and desktop defaults available.\n";
            return 0;
        }
        Fixture f;
        const auto withoutVendor = readUdcapPreferences(f.sources);
        require(!withoutVendor.found && withoutVendor.receiver[0].empty() && withoutVendor.receiver[1].empty(),
                "OpenCap starts with its own defaults when UDCAP configuration files are absent");
        Preferences preferences;
        preferences.quickStick = true;
        preferences.steamLifecycle = true;
        preferences.hands[0].tracker = 8;
        require(saveSettings(f.root, preferences), "Save optional shortcut and SteamVR lifecycle settings");
        std::ifstream saved(f.root / L"settings.txt");
        int format = 0;
        saved >> format;
        saved.close();
        const auto restored = loadSettings(f.root);
        require(format == 3 && restored.quickStick && restored.steamLifecycle == true && restored.hands[0].tracker == 8,
                "New preferences retain a file version the previous application can read");
        require(!steamLifecycleEnabled(f.root, {}), "Portable desktop keeps autostart off by default");
        put(f.root / L"steamvr-switch.json", "{}");
        require(steamLifecycleEnabled(f.root, {}), "Setup enables desktop autostart when no choice was saved");
        preferences.steamLifecycle = false;
        require(saveSettings(f.root, preferences) && !steamLifecycleEnabled(f.root, loadSettings(f.root)),
                "A saved opt-out overrides the setup default on the next SteamVR start");
        put(f.sources.desktopDefaults, R"xml(<configuration><appSettings>
          <add key="L_Rocky_X_Min" value="600"/><add key="L_Rocky_X_Center" value="1800"/>
          <add key="L_Rocky_X_Max" value="3800"/><add key="L_Vibartion_Enable" value="True"/>
          <add key="L_TriggerValue_Min" value="25"/><add key="L_TriggerValue" value="50"/>
          <add key="L_Coefficient_Index1_Pitch" value="1"/><add key="L_Balance_Index1_Pitch" value=".5"/>
          </appSettings></configuration>)xml");
        put(f.sources.steamvrDefaults, R"json({"driver_UDCAP":{"left_enabled":true,"feedback_enabled":true},
          "pose_settings":{"left_x_offset_position":0.1,"right_x_offset_position":-0.1,"pose_time_offset":-0.01}})json");
        auto data = readUdcapPreferences(f.sources);
        require(data.found && !data.saved && data.receiver[0].controller.at("xCenterData") == 1800,
                "Factory defaults load without a user profile");
        require(closeTo(data.hands[0].position.x, .1) && closeTo(data.hands[0].poseTimeOffset, -.01),
                "Factory driver offsets and time are read");

        put(f.sources.desktopSettings, R"json({"SchemaVersion":1,"Settings":{
          "L_Rocky_X_Center":"1702","L_TriggerValue_Min":"20","L_Vibartion_Enable":false,"L_ThumbOn":"False",
          "L_Coefficient_Index1_Pitch":"1.25","L_Offset_Index1_Pitch":"-3","L_Balance_Index1_Pitch":"0.7",
          "L_Output_Middle1_Yaw":"5","Controller_Priority":"True","Controller_Module":"True",
          "VMC_State":"True","VMC_IP":"127.0.0.2","VMC_PORT":"39539",
          "OSC_State":"True","OSC_IP":"127.0.0.3","OSC_PORT":"9000",
          "VRSpaceCustom":"True","VRSpaceCustomValueStr":"-.1,-.2,-.3,10,-20,30,.1,.2,.3,40,50,60,"
        }})json");
        data = readUdcapPreferences(f.sources);
        require(data.saved && data.quickStick && !data.hands[0].haptics && !data.hands[0].thumbEnabled && data.hands[1].highPriority,
                "Saved desktop values override installed defaults");
        require(data.receiver[0].controller.at("xCenterData") == 1702
            && data.receiver[0].controller.at("xMaxData") == 3800
            && closeTo(data.receiver[0].controller.at("triggerMin"), .2),
            "Raw joystick calibration and percent thresholds are mapped");
        require(data.receiver[0].hasCurves && closeTo(data.receiver[0].curves[6].min, -100)
            && closeTo(data.receiver[0].curves[6].offset, -3) && closeTo(data.receiver[0].curves[6].balance, .7)
            && closeTo(data.receiver[0].curves[11].offset, 5),
            "Hand response gain, balance and offsets use the correct channels");
        require(data.stream.address == "127.0.0.2" && data.stream.oscAddress == "127.0.0.3"
            && data.stream.vmcPort == 39539 && data.stream.oscPort == 9000,
            "VMC and OSC keep independent destinations");
        require(closeTo(data.hands[0].position.z, -.3) && closeTo(data.hands[0].rotation.y, -50),
                "Desktop pose fallback uses the correct hand order and axes");

        std::ofstream registry(f.sources.steamvrRegistry);
        registry << "{\"config\":[" << std::quoted(f.root.generic_string()) << "]}";
        registry.close();
        put(f.vr, R"json({"driver_UDCAP":{"skeleton_only":true,"right_enabled":false},
          "pose_settings":{"left_x_offset_position":-0.3,"left_z_offset_position":0.4,
          "controller_override":true,"controller_override_left":8}})json");
        data = readUdcapPreferences(f.sources);
        auto hands = std::array<HandSettings, handCount>{};
        hands[0].buttonMap[0] = 2;
        StreamSettings stream;
        applyUdcapPreferences(data, hands, stream);
        require(closeTo(hands[0].position.x, -.3) && closeTo(hands[0].position.z, .4)
            && hands[0].tracker == 8 && hands[0].skeletonOnly && !hands[1].enabled,
            "Active UDCAP driver settings take precedence over custom desktop offsets");
        require(hands[0].buttonMap[0] == 2, "OpenCap-only mappings survive migration");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
