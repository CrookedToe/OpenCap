#include "firmware_image.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <vector>

namespace opencap {
namespace {
constexpr std::size_t maximumImage = 2 * 1024 * 1024;
constexpr std::uint32_t imageMagic = 0x96f3b83d;
constexpr std::uint16_t protectedMagic = 0x6908, regularMagic = 0x6907;
using Bytes = std::span<const std::uint8_t>;
using Digest = std::array<std::uint8_t, 32>;

std::uint16_t u16(Bytes data, std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset] | (data[offset+1] << 8));
}

std::uint32_t u32(Bytes data, std::size_t offset) {
    return u16(data, offset) | (static_cast<std::uint32_t>(u16(data, offset+2)) << 16);
}

Digest sha256(Bytes data) {
    struct MachineHash {
        BCRYPT_ALG_HANDLE handle{};
        ~MachineHash() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
    } algorithm;
    if (BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Windows could not initialize SHA-256.");
    Digest hash{};
    if (BCryptHash(algorithm.handle, nullptr, 0, const_cast<PUCHAR>(data.data()),
                   static_cast<ULONG>(data.size()), hash.data(), static_cast<ULONG>(hash.size())) < 0)
        throw std::runtime_error("Windows could not hash the firmware file.");
    return hash;
}

template<class Record>
void readTable(Bytes data, std::uint16_t expectedMagic, Record visit) {
    if (data.size() < 4 || u16(data, 0) != expectedMagic || u16(data, 2) != data.size())
        throw std::runtime_error("The MCUboot metadata table is invalid.");
    for (std::size_t i = 4; i < data.size();) {
        if (data.size() - i < 4) throw std::runtime_error("A metadata record header is incomplete.");
        const auto type = u16(data, i);
        const auto length = u16(data, i+2);
        i += 4;
        if (length > data.size() - i) throw std::runtime_error("A metadata record extends beyond the image.");
        visit(type, data.subspan(i, length));
        i += length;
    }
}
}

FirmwareImage inspectFirmware(Bytes image) {
    if (image.size() < 32 || image.size() > maximumImage || u32(image, 0) != imageMagic)
        throw std::runtime_error("Choose a supported MCUboot firmware image.");
    const auto header = u16(image, 8);
    const auto protectedBytes = u16(image, 10);
    const auto payload = u32(image, 12);
    const auto flags = u32(image, 16);
    const auto signedBytes = static_cast<std::uint64_t>(header) + payload + protectedBytes;
    if (header < 32 || payload == 0 || signedBytes > image.size())
        throw std::runtime_error("The firmware header declares an invalid payload size.");
    if (flags != 0) throw std::runtime_error("This firmware uses image flags unsupported by the UDCAP inspector.");
    if (protectedBytes)
        readTable(image.subspan(header+payload, protectedBytes), protectedMagic, [](auto, auto) {});

    Bytes expected;
    bool keyHash = false;
    std::optional<ImageSignature> signature;
    readTable(image.subspan(static_cast<std::size_t>(signedBytes)), regularMagic,
        [&](std::uint16_t type, Bytes data) {
            if (type == 0x10) {
                if (!expected.empty() || data.size() != Digest{}.size())
                    throw std::runtime_error("The image has an invalid SHA-256 record.");
                expected = data;
            } else if (type == 0x01) {
                if (keyHash || data.size() != Digest{}.size())
                    throw std::runtime_error("The image has an invalid signing-key identifier.");
                keyHash = true;
            } else if (type == 0x20 || type == 0x22) {
                if (signature || (type == 0x20 && data.size() != 256) ||
                    (type == 0x22 && (data.size() < 64 || data.size() > 80)))
                    throw std::runtime_error("The image has an invalid signature record.");
                signature = type == 0x20 ? ImageSignature::rsa2048 : ImageSignature::ecdsaP256;
            }
        });
    if (expected.empty() || !keyHash || !signature)
        throw std::runtime_error("The image is missing UDCAP's hash or signing metadata.");
    const auto digest = sha256(image.first(static_cast<std::size_t>(signedBytes)));
    if (!std::ranges::equal(digest, expected)) throw std::runtime_error("The firmware payload SHA-256 does not match.");
    return {image[20], image[21], u16(image, 22), u32(image, 24), payload, *signature};
}

FirmwareImage readFirmware(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximumImage) throw std::runtime_error("Could not read a firmware file of the supported size.");
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> contents(static_cast<std::size_t>(size));
    if (!input.read(reinterpret_cast<char*>(contents.data()), static_cast<std::streamsize>(contents.size())) ||
        input.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("The firmware file could not be read completely.");
    return inspectFirmware(contents);
}

FirmwareTarget firmwareTarget(const std::filesystem::path& path) {
    auto name = path.filename().wstring();
    for (auto& c : name) c = static_cast<wchar_t>(std::towlower(c));
    if (name == L"nrf52832_rx_app_update.bin") return FirmwareTarget::receiver;
    if (name == L"nrf52840_tx_left_app_update.bin") return FirmwareTarget::leftGlove;
    if (name == L"nrf52840_tx_right_app_update.bin") return FirmwareTarget::rightGlove;
    return FirmwareTarget::unknown;
}
}
