#pragma once

#include "model.hpp"
#include "stream.hpp"

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace opencap {
struct ReceiverPreferences {
    std::map<std::string, double> controller;
    struct Curve {
        double min{}, max{}, balance{.5}, offset{};
    };
    std::array<Curve, 21> curves{};
    bool hasCurves{};
    bool empty() const { return controller.empty() && !hasCurves; }
};

struct UdcapSources {
    std::filesystem::path desktopDefaults;
    std::filesystem::path desktopSettings;
    std::filesystem::path steamvrDefaults;
    std::filesystem::path steamvrRegistry;
};

struct UdcapPreferences {
    bool found{};
    bool saved{};
    bool quickStick{};
    std::array<HandSettings, handCount> hands{};
    StreamSettings stream;
    std::array<ReceiverPreferences, handCount> receiver;
};

UdcapSources findUdcapSources(const std::filesystem::path& localAppData);
UdcapPreferences readUdcapPreferences(const UdcapSources& sources);
void applyUdcapPreferences(const UdcapPreferences& source,
                          std::array<HandSettings, handCount>& hands, StreamSettings& stream);
}
