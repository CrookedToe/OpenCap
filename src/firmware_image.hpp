#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace opencap {
enum class ImageSignature { rsa2048, ecdsaP256 };
enum class FirmwareTarget { unknown, receiver, leftGlove, rightGlove };
struct FirmwareImage {
    std::uint8_t major{}, minor{};
    std::uint16_t revision{};
    std::uint32_t build{}, payloadBytes{};
    ImageSignature signature{};
    bool signatureVerified{};
};

FirmwareImage inspectFirmware(std::span<const std::uint8_t> image, std::span<const std::uint8_t> trustedKey = {});
FirmwareImage readFirmware(const std::filesystem::path& path, const std::filesystem::path& trustedKey = {});
FirmwareTarget firmwareTarget(const std::filesystem::path& path);

struct FirmwareUpdateImage {
    FirmwareTarget target{};
    FirmwareImage version;
    std::vector<std::uint8_t> contents;
};
FirmwareUpdateImage readUpdateImage(const std::filesystem::path& path);
}
