#pragma once

#include "model.hpp"
#include "udcap_preferences.hpp"
#include "firmware_image.hpp"
#include "firmware_transfer.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <optional>

namespace opencap {
struct WirelessFirmwareTarget {
    std::wstring port;
    std::string serial;
    Side side;
};
void updateWiredDevice(const FirmwareUpdateImage& image, const FirmwareProgress& progress);
void updateWirelessGlove(const FirmwareUpdateImage& image, const WirelessFirmwareTarget& target,
                         const FirmwareProgress& progress);

class Receiver {
public:
    Receiver(const std::filesystem::path& preferences, std::array<ReceiverPreferences, handCount> defaults,
             std::uint64_t importRevision);
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    std::array<Hand, handCount> read() const;
    std::optional<CalibrationSamples> calibration(Side side) const;
    std::optional<HandCurves> handCurves(Side side) const;
    std::optional<WirelessFirmwareTarget> wirelessFirmwareTarget(Side side) const;
    bool submit(const Command& command);
    void importPreferences(std::array<ReceiverPreferences, handCount> defaults, std::uint64_t importRevision);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
