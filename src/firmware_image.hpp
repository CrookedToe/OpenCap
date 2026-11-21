#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

namespace opencap {
enum class ImageSignature { rsa2048, ecdsaP256 };
enum class FirmwareTarget { unknown, receiver, leftGlove, rightGlove };
struct FirmwareImage {
    std::uint8_t major{}, minor{};
    std::uint16_t revision{};
    std::uint32_t build{}, payloadBytes{};
    ImageSignature signature{};
};

FirmwareImage inspectFirmware(std::span<const std::uint8_t> image);
FirmwareImage readFirmware(const std::filesystem::path& path);
FirmwareTarget firmwareTarget(const std::filesystem::path& path);
}
