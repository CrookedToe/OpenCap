#pragma once

#include "model.hpp"
#include "stream.hpp"

#include <filesystem>
#include <optional>

namespace opencap {
struct Preferences {
    std::array<HandSettings, handCount> hands{};
    StreamSettings stream{};
    bool hotkey{};
    std::uint64_t udcapImport{};
    bool quickStick{};
    std::optional<bool> steamLifecycle;
    bool includePrereleases{};
};

std::filesystem::path executableFolder();
std::filesystem::path configFolder(bool demo);
bool validPose(const HandSettings& hand);
Preferences loadSettings(const std::filesystem::path& path);
bool steamLifecycleEnabled(const std::filesystem::path& path, const Preferences& settings);
bool saveSettings(const std::filesystem::path& path, const Preferences& settings);
bool launchUpdater(bool automatic, bool includePrereleases);
std::uint64_t nextImport(std::uint64_t previous);
}
