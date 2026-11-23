#include "firmware_image.hpp"
#include "firmware_transfer.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace opencap;

namespace {
void require(bool pass, const char* message) { if (!pass) throw std::runtime_error(message); }

std::vector<std::uint8_t> hex(std::string_view text) {
    const auto digit = [](char value) { return value <= '9' ? value - '0' : value - 'a' + 10; };
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < text.size(); i += 2)
        bytes.push_back(static_cast<std::uint8_t>(digit(text[i]) * 16 + digit(text[i + 1])));
    return bytes;
}

std::vector<std::uint8_t> fixture(std::uint8_t type, std::span<const std::uint8_t> keyHash,
                                  std::span<const std::uint8_t> signature) {
    std::vector<std::uint8_t> data{
        0x3d,0xb8,0xf3,0x96, 0,0,0,0, 32,0,0,0, 4,0,0,0, 0,0,0,0,
        0,1,2,0, 3,0,0,0, 0,0,0,0, 17,34,51,68,
        0x07,0x69,0,0, 0x10,0,32,0,
        0x4b,0x8f,0xdb,0x36,0x63,0x0a,0x3e,0x55,0x91,0x55,0x68,0xbb,0xb5,0xe5,0x79,0xd4,
        0xa9,0x92,0xf4,0xc4,0x3c,0x6e,0xfe,0x64,0x38,0x8b,0xe7,0x20,0x46,0x14,0x6f,0x64,
        0x01,0,32,0};
    data.insert(data.end(), keyHash.begin(), keyHash.end());
    data.insert(data.end(), {type, 0, static_cast<std::uint8_t>(signature.size()),
                                  static_cast<std::uint8_t>(signature.size() >> 8)});
    data.insert(data.end(), signature.begin(), signature.end());
    const auto tableSize = data.size() - 36;
    data[38] = static_cast<std::uint8_t>(tableSize);
    data[39] = static_cast<std::uint8_t>(tableSize >> 8);
    return data;
}

bool rejects(std::span<const std::uint8_t> data, std::span<const std::uint8_t> key = {},
             std::string_view error = {}) {
    try { inspectFirmware(data, key); }
    catch (const std::runtime_error& failure) { return std::string_view(failure.what()).find(error) != error.npos; }
    return false;
}

void checkSignatures(const std::vector<std::uint8_t>& rsaImage) {
    // These signatures were produced independently over the fixture header and payload.
    constexpr std::string_view rsaPem =
        "-----BEGIN PUBLIC KEY-----\n"
        "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAoN844iPRDQ2cPpRj+apK\n"
        "pAlssFJCClVFAsG9b9EuiYYWW+EOMOkF7OIvOa+KeguTtOTHDf3c1ioMqXRZshpC\n"
        "/faNZO7ICFM4haUhwtQM0eN/cwse+CF1HazF5wypTpS3zq6eu+yTetzxaHi2odnq\n"
        "lE4Vly1r9xZKK+trc88PVFuItDzRwtN59BX6ZMLaYePK7h32JPoXKFCdH3doqcOp\n"
        "ZZBO7UY0HVkMZqH9aOis9VXzY8qKy7LZpcWSEOjzJbWExFaLJrP+GEcktXyQdGDh\n"
        "zUaZfr1qzs4P78j2KA762nV63s3R1/RUGpavGn2f+Zp89yQbJoul39tGRw3Ro+x2\n"
        "6wIDAQAB\n"
        "-----END PUBLIC KEY-----\n";
    const std::span rsaKey{reinterpret_cast<const std::uint8_t*>(rsaPem.data()), rsaPem.size()};
    require(inspectFirmware(rsaImage, rsaKey).signatureVerified, "Verify an RSA-PSS signature with a PEM public key");
    auto corrupt = rsaImage;
    corrupt.back() ^= 1;
    require(rejects(corrupt, rsaKey, "RSA firmware signature"), "Reject a modified RSA signature");

    const auto ecKey = hex(
        "3059301306072a8648ce3d020106082a8648ce3d030107034200048e484ef4cefbdeb198f1d53e4c81435e928455f"
        "18a3609acd591bdb26452aa64a36f33f81e0c09a151a4a725e1020055de95f1ce3b6ffd98cddc561229d816aa");
    const auto ecHash = hex("37e9927289e4461dc624b31c7af0f606dc5626cac3821a390c1e739194f078ab");
    auto ecSignature = hex(
        "30440220241d8545adbca98696961db5e9b95d828984e3bf1ba77dd6af4c7a6d8d15851e"
        "02201b50f90fc4f4bf2eab2a6b514044a37e0bc8cbc739db15501fa942640c5e5923");
    require(inspectFirmware(fixture(0x22, ecHash, ecSignature), ecKey).signatureVerified,
            "Verify an ECDSA signature with a DER public key");
    ecSignature.resize(72);
    const auto ecImage = fixture(0x22, ecHash, ecSignature);
    require(inspectFirmware(ecImage, ecKey).signatureVerified, "Accept legacy zero padding on an ECDSA signature");
    corrupt = ecImage;
    corrupt.back() = 1;
    require(rejects(corrupt, ecKey, "ECDSA signature encoding"), "Reject nonzero ECDSA padding");
    corrupt = ecImage;
    corrupt[80] ^= 1;
    require(rejects(corrupt, ecKey, "signing-key identifier"), "Reject a mismatched key identifier");
}

void checkWiredTransfer() {
    require(wiredGloveSide("channel=50,mode=2,device=glove,sn=ACSTDE4FG540004L0\r") == 'L' &&
            wiredGloveSide("channel=50,mode=2,device=glove,sn=ACSTDE4FG540004R0") == 'R' &&
            !wiredGloveSide("channel=50,mode=2,device=glove,sn=AB") &&
            !wiredGloveSide("channel=50,sn=ACSTDE4FG540004L0"), "Identify the USB glove before sending firmware");
    const auto image = hex("112233");
    const auto ack = hex("19a600020301c0806a");
    std::size_t offset = 0;
    bool wrote = false;
    unsigned progress = 0;
    sendFirmware(image, [&](auto frame) {
        require(!wrote && std::ranges::equal(frame, hex("a5000d030000000003000100001122334f5c5a")),
                "Match the wired OTA header, size, chunk index, payload and Modbus CRC");
        wrote = true;
    }, [&](auto buffer) {
        const auto bytes = std::span(ack).subspan(offset, std::min<std::size_t>(3, ack.size() - offset));
        std::ranges::copy(bytes, buffer.begin());
        offset += bytes.size();
        return bytes.size();
    }, [&](unsigned value) { progress = value; });
    require(wrote && progress == 100, "Handle a fragmented bootloader reply after unrelated bytes");

    auto reply = hex("a600040301000051a06aa600040301000051a06aa600040301010050306a");
    const std::vector<std::uint8_t> twoBlocks(401, 0x65);
    auto block = 0u;
    sendFirmware(twoBlocks, [&](auto frame) {
        require(frame[10] == 2 && frame[12] == block, "Send each block once and in sequence");
        ++block;
    }, [&](auto buffer) {
        std::ranges::copy(reply, buffer.begin());
        const auto size = reply.size();
        reply.clear();
        return size;
    }, [&](unsigned value) { progress = value; });
    require(block == 2 && progress == 100, "Ignore a stale indexed response while transferring");

    bool rejected = false;
    try {
        sendFirmware(image, [](auto) {}, [](auto buffer) {
            const auto failure = hex("a60002030400836a");
            std::ranges::copy(failure, buffer.begin());
            return failure.size();
        }, [](unsigned) {});
    } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()).find("rejected") != std::string_view::npos;
    }
    require(rejected, "Stop on bootloader rejection");
}

void checkWirelessTransfer() {
    std::vector<std::uint8_t> image(161, 0x21);
    image.back() = 0x17;
    unsigned block = 0, completed = 0;
    sendFirmware(image, [&](auto frame) {
        require(block < 2 && frame.size() == 176 && frame.back() == 0x5a, "Wireless firmware writes fixed-size frames");
        const auto prefix = hex(block ? "55aa01de00000000a1000200011700" : "55aa01de00000000a1000200002121");
        const auto crc = hex(block ? "2553" : "3681");
        require(std::ranges::equal(frame.first(prefix.size()), prefix) && std::ranges::equal(frame.subspan(173, 2), crc),
                "Wireless firmware uses the radio command, big-endian block index and zero-padded final payload");
        ++block;
    }, [&](auto buffer) {
        const auto reply = hex(block == 1 ? "a50184ffa60003de0100106a6a" : "a60003de0100106a6aa6000303010141906a");
        std::ranges::copy(reply, buffer.begin());
        return reply.size();
    }, [&](unsigned value) { completed = value; }, FirmwareTransport::wireless);
    require(block == 2 && completed == 100, "Advance radio updates only on acknowledgments for the current block");
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc > 1) {
            for (int i = 1; i < argc; ++i) {
                const auto info = readFirmware(argv[i]);
                const auto target = firmwareTarget(argv[i]);
                require(target != FirmwareTarget::unknown, "Installed image has a known UDCAP target name");
                require(readUpdateImage(argv[i]).target == target, "Match original firmware images by full digest");
                std::wcout << std::filesystem::path(argv[i]).filename().wstring() << L" " << info.major + 0u << L"."
                           << info.minor + 0u << L"." << info.revision << L"+" << info.build << L" hash matches\n";
            }
            return 0;
        }
        const auto good = fixture(0x20, hex("ddcd5b1eab303e257b9cc7b20d21b728697d587a684192318fc4da6b836ea4b3"),
            hex("507602dbca73fe5d5afccc31304e84007bd26f50212223db0bd3459295b825aa7f2191a2d305229e739133a66a515fb62"
                "b2786048bc375a94b087eedd680d15a196bb991816eec701a298278f8dc587775425922c55e7cc6d5c923d426cb1cc55c"
                "6147656f9d88e1a4c2d66b9ad66440b9997536211893e87c025e20422158f408d1799522e860631f9b9942dee675249c"
                "043198a68c8d8a77d6c4c5dfd2cb4759552661bd2f329c935e0b949b9ba75ba1523d922b45a719ee51bbf456acb900"
                "94ee98f1c57c9ebcf8d2289d716206fba55970e383161fb1de7b006e11fac13019e6f0a3ad3d856c2cae822b32493"
                "ab8d8d5afabbb9a9542afb1570304b0e5c8"));
        const auto info = inspectFirmware(good);
        require(info.minor == 1 && info.revision == 2 && info.build == 3 && info.payloadBytes == 4,
                "Read the MCUboot version and payload size");
        require(info.signature == ImageSignature::rsa2048, "Recognize the installed receiver signature scheme");
        require(!info.signatureVerified, "Require a trusted public key to report a verified signature");
        checkSignatures(good);
        checkWiredTransfer();
        checkWirelessTransfer();
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
