#include "desktop_preferences.hpp"

#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>

namespace opencap {
std::filesystem::path executableFolder() {
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path configFolder(bool demo) {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) return {};
    std::filesystem::path folder(path);
    CoTaskMemFree(path);
    folder /= L"OpenCap";
    if (demo) folder /= L"Demo";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    return error ? std::filesystem::path{} : folder;
}

bool validPose(const HandSettings& hand) {
    const auto p = hand.position;
    const auto r = hand.rotation;
    return (hand.tracker == noTracker || (hand.tracker >= 1 && hand.tracker < 64))
        && std::abs(p.x) <= 1 && std::abs(p.y) <= 1 && std::abs(p.z) <= 1
        && std::abs(r.x) <= 360 && std::abs(r.y) <= 360 && std::abs(r.z) <= 360;
}

Preferences loadSettings(const std::filesystem::path& path) {
    Preferences settings;
    if (path.empty()) return settings;
    std::ifstream input(path / "settings.txt");
    input.imbue(std::locale::classic());
    int version = 0;
    if (!(input >> version) || version < 1 || version > 5) return {};
    for (auto& hand : settings.hands) {
        auto& p = hand.position;
        auto& r = hand.rotation;
        if (!(input >> hand.tracker >> p.x >> p.y >> p.z >> r.x >> r.y >> r.z
                    >> hand.enabled >> hand.joystick >> hand.haptics) || !validPose(hand)) return {};
        if (version >= 2) {
            std::string serial;
            if (!(input >> hand.highPriority >> std::quoted(serial)) || serial.size() >= hand.trackerSerial.size())
                return {};
            std::copy(serial.begin(), serial.end(), hand.trackerSerial.begin());
            for (auto& mapping : hand.buttonMap)
                if (!(input >> mapping) || mapping > disabledButton) return {};
        }
        if (version >= 3 && (!(input >> hand.triggerEnabled >> hand.gripEnabled >> hand.padEnabled >>
            hand.thumbEnabled >> hand.skeletonOnly >> hand.poseTimeOffset) || !(std::abs(hand.poseTimeOffset) <= .25f)))
            return {};
    }
    auto& s = settings.stream;
    if (!(input >> std::quoted(s.address) >> s.vmcPort >> s.oscPort >> settings.hotkey)
        || s.vmcPort > 65535 || s.oscPort > 65535) return {};
    s.oscAddress = s.address;
    if (version >= 3 && !(input >> std::quoted(s.oscAddress) >> settings.udcapImport)) return {};
    if (version >= 3 && (input >> std::ws) && !input.eof() && !(input >> settings.quickStick)) return {};
    if (version >= 3 && (input >> std::ws) && !input.eof() && !(input >> settings.steamLifecycle)) return {};
    return settings;
}

bool saveSettings(const std::filesystem::path& path, const Preferences& settings) {
    if (path.empty()) return false;
    const auto tmp = path / "settings.tmp";
    {
        std::ofstream output(tmp, std::ios::trunc);
        output.imbue(std::locale::classic());
        // Older OpenCap builds can still read the original fields after rollback.
        output << "3\n" << std::setprecision(8);
        for (const auto& hand : settings.hands) {
            const auto p = hand.position, r = hand.rotation;
            output << hand.tracker << ' ' << p.x << ' ' << p.y << ' ' << p.z << ' '
                   << r.x << ' ' << r.y << ' ' << r.z << ' '
                   << hand.enabled << ' ' << hand.joystick << ' ' << hand.haptics << ' '
                   << hand.highPriority << ' ' << std::quoted(hand.trackerSerial.data());
            for (const auto mapping : hand.buttonMap) output << ' ' << mapping;
            output << ' ' << hand.triggerEnabled << ' ' << hand.gripEnabled << ' ' << hand.padEnabled << ' '
                   << hand.thumbEnabled << ' ' << hand.skeletonOnly << ' ' << hand.poseTimeOffset << '\n';
        }
        const auto& s = settings.stream;
        output << std::quoted(s.address) << ' ' << s.vmcPort << ' ' << s.oscPort << ' ' << settings.hotkey
               << ' ' << std::quoted(s.oscAddress) << ' ' << settings.udcapImport << ' ' << settings.quickStick
               << ' ' << settings.steamLifecycle << '\n';
        output.close();
        if (!output) return false;
    }
    return MoveFileExW(tmp.c_str(), (path / "settings.txt").c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

std::uint64_t nextImport(std::uint64_t previous) {
    const auto clock = std::chrono::system_clock::now().time_since_epoch();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(clock).count();
    return (std::max)(previous + 1, static_cast<std::uint64_t>(milliseconds));
}

}
