#pragma once

#include "model.hpp"
#include "stream.hpp"

#include <filesystem>

namespace opencap {
struct Preferences {
    std::array<HandSettings, handCount> hands{};
    StreamSettings stream{};
    bool hotkey{};
    std::uint64_t udcapImport{};
    bool quickStick{};
};

std::filesystem::path executableFolder();
std::filesystem::path configFolder(bool demo);
bool validPose(const HandSettings& hand);
Preferences loadSettings(const std::filesystem::path& path);
bool saveSettings(const std::filesystem::path& path, const Preferences& settings);
std::uint64_t nextImport(std::uint64_t previous);
}
