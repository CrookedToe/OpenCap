#include "firmware_image.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace opencap;

namespace {
void require(bool pass, const char* message) { if (!pass) throw std::runtime_error(message); }

std::vector<std::uint8_t> fixture() {
    std::vector<std::uint8_t> data{
        0x3d,0xb8,0xf3,0x96, 0,0,0,0, 32,0,0,0, 4,0,0,0, 0,0,0,0,
        0,1,2,0, 3,0,0,0, 0,0,0,0, 17,34,51,68,
        0x07,0x69,0x50,0x01, 0x10,0,32,0,
        0x4b,0x8f,0xdb,0x36,0x63,0x0a,0x3e,0x55,0x91,0x55,0x68,0xbb,0xb5,0xe5,0x79,0xd4,
        0xa9,0x92,0xf4,0xc4,0x3c,0x6e,0xfe,0x64,0x38,0x8b,0xe7,0x20,0x46,0x14,0x6f,0x64,
        0x01,0,32,0};
    data.resize(data.size() + 32);
    const std::array<std::uint8_t, 4> signature{0x20,0,0,1};
    data.insert(data.end(), signature.begin(), signature.end());
    data.resize(data.size() + 256);
    return data;
}

bool rejects(std::span<const std::uint8_t> data) {
    try { inspectFirmware(data); }
    catch (const std::runtime_error&) { return true; }
    return false;
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc > 1) {
            for (int i = 1; i < argc; ++i) {
                const auto info = readFirmware(argv[i]);
                require(firmwareTarget(argv[i]) != FirmwareTarget::unknown, "Installed image has a known UDCAP target name");
                std::wcout << std::filesystem::path(argv[i]).filename().wstring() << L" " << info.major + 0u << L"."
                           << info.minor + 0u << L"." << info.revision << L"+" << info.build << L" hash matches\n";
            }
            return 0;
        }
        const auto good = fixture();
        const auto info = inspectFirmware(good);
        require(info.minor == 1 && info.revision == 2 && info.build == 3 && info.payloadBytes == 4,
                "Read the MCUboot version and payload size");
        require(info.signature == ImageSignature::rsa2048, "Recognize the installed receiver signature scheme");
        auto corrupt = good;
        corrupt[32] ^= 1;
        require(rejects(corrupt), "Reject corrupted firmware payload");
        corrupt = good;
        corrupt[13] = 0xff;
        require(rejects(corrupt), "Reject an out-of-bounds payload length");
        corrupt = good;
        corrupt[41] = 1;
        require(rejects(corrupt), "Reject firmware missing its payload hash");
        require(rejects(std::span{good}.first(good.size()-1)), "Reject an incomplete metadata table");
        require(firmwareTarget(L"NRF52840_TX_LEFT_APP_UPDATE.BIN") == FirmwareTarget::leftGlove,
                "Use a case-insensitive vendor filename to identify the declared target");
        require(firmwareTarget(L"renamed.bin") == FirmwareTarget::unknown,
                "Arbitrary filenames do not establish a device target");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
