#include "firmware_image.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace opencap {
namespace {
constexpr std::size_t maximumImage = 2 * 1024 * 1024;
constexpr std::size_t maximumKey = 16 * 1024;
constexpr std::uint32_t imageMagic = 0x96f3b83d;
constexpr std::uint16_t protectedMagic = 0x6908, regularMagic = 0x6907;
using Bytes = std::span<const std::uint8_t>;
using Digest = std::array<std::uint8_t, 32>;

struct Algorithm {
    BCRYPT_ALG_HANDLE value{};
    explicit Algorithm(const wchar_t* name) {
        if (BCryptOpenAlgorithmProvider(&value, name, nullptr, 0) < 0)
            throw std::runtime_error("Windows could not open the cryptographic provider.");
    }
    ~Algorithm() { BCryptCloseAlgorithmProvider(value, 0); }
    Algorithm(const Algorithm&) = delete;
    Algorithm& operator=(const Algorithm&) = delete;
};

struct PublicKey {
    BCRYPT_KEY_HANDLE value{};
    ~PublicKey() { if (value) BCryptDestroyKey(value); }
    PublicKey() = default;
    PublicKey(const PublicKey&) = delete;
    PublicKey& operator=(const PublicKey&) = delete;
};

std::uint16_t u16(Bytes data, std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset] | (data[offset+1] << 8));
}

std::uint32_t u32(Bytes data, std::size_t offset) {
    return u16(data, offset) | (static_cast<std::uint32_t>(u16(data, offset+2)) << 16);
}

Digest sha256(Bytes data) {
    Algorithm algorithm(BCRYPT_SHA256_ALGORITHM);
    Digest hash{};
    if (BCryptHash(algorithm.value, nullptr, 0, const_cast<PUCHAR>(data.data()),
                   static_cast<ULONG>(data.size()), hash.data(), static_cast<ULONG>(hash.size())) < 0)
        throw std::runtime_error("Windows could not hash the firmware file.");
    return hash;
}

std::vector<std::uint8_t> publicKeyDer(Bytes input) {
    if (input.empty() || input.size() > maximumKey) throw std::runtime_error("Choose a public key in PEM or DER format.");
    if (input.front() == 0x30) return {input.begin(), input.end()};
    const std::string_view text(reinterpret_cast<const char*>(input.data()), input.size());
    constexpr auto format = CRYPT_STRING_BASE64HEADER | CRYPT_STRING_STRICT;
    DWORD size{};
    if ((!text.starts_with("-----BEGIN PUBLIC KEY-----") && !text.starts_with("-----BEGIN RSA PUBLIC KEY-----")) ||
        !CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), format, nullptr, &size, nullptr, nullptr))
        throw std::runtime_error("Choose a public key in PEM or DER format.");
    std::vector<std::uint8_t> der(size);
    if (!CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), format,
                             der.data(), &size, nullptr, nullptr))
        throw std::runtime_error("The public key PEM data is invalid.");
    der.resize(size);
    return der;
}

std::array<std::uint8_t, 64> decodeEcdsaSignature(Bytes signature) {
    constexpr auto invalid = "The ECDSA signature encoding is invalid.";
    if (signature.size() < 2 || signature[0] != 0x30 || signature[1] > signature.size() - 2)
        throw std::runtime_error(invalid);
    auto sequence = signature.subspan(2, signature[1]);
    const auto padding = signature.subspan(2 + sequence.size());
    if (std::ranges::any_of(padding, [](auto byte) { return byte != 0; })) throw std::runtime_error(invalid);
    std::array<std::uint8_t, 64> result{};
    for (std::size_t i = 0; i < result.size(); i += 32) {
        if (sequence.size() < 3 || sequence.front() != 0x02 || !sequence[1] || sequence[1] > sequence.size() - 2)
            throw std::runtime_error(invalid);
        auto integer = sequence.subspan(2, sequence[1]);
        sequence = sequence.subspan(2 + integer.size());
        if (integer.front() & 0x80) throw std::runtime_error(invalid);
        if (integer.size() > 1 && integer.front() == 0) {
            if (!(integer[1] & 0x80)) throw std::runtime_error(invalid);
            integer = integer.subspan(1);
        }
        if (integer.size() > 32) throw std::runtime_error(invalid);
        std::ranges::copy(integer, result.begin() + i + 32 - integer.size());
    }
    if (!sequence.empty()) throw std::runtime_error(invalid);
    return result;
}

void verifySignature(ImageSignature scheme, Digest digest, Bytes keyHash, Bytes signature, Bytes trustedKey) {
    auto der = publicKeyDer(trustedKey);
    struct DecodedKey {
        CERT_PUBLIC_KEY_INFO* info{};
        ~DecodedKey() { if (info) LocalFree(info); }
    } decoded;
    DWORD bytes{};
    CryptDecodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, der.data(), static_cast<DWORD>(der.size()),
                        CRYPT_DECODE_ALLOC_FLAG, nullptr, &decoded.info, &bytes);
    if (scheme == ImageSignature::rsa2048) {
        CERT_PUBLIC_KEY_INFO rsa{};
        rsa.Algorithm.pszObjId = const_cast<LPSTR>(szOID_RSA_RSA);
        rsa.PublicKey = {static_cast<DWORD>(der.size()), der.data(), 0};
        if (decoded.info) {
            if (std::string_view(decoded.info->Algorithm.pszObjId) != szOID_RSA_RSA)
                throw std::runtime_error("The image requires an RSA public key.");
            rsa.PublicKey = decoded.info->PublicKey;
        }
        if (!std::ranges::equal(sha256({rsa.PublicKey.pbData, rsa.PublicKey.cbData}), keyHash))
            throw std::runtime_error("The public key does not match the image's signing-key identifier.");
        PublicKey key;
        if (!CryptImportPublicKeyInfoEx2(X509_ASN_ENCODING, &rsa, CRYPT_OID_INFO_PUBKEY_SIGN_KEY_FLAG, nullptr, &key.value))
            throw std::runtime_error("Windows could not import the RSA public key.");
        DWORD bits{}, received{};
        if (BCryptGetProperty(key.value, BCRYPT_KEY_LENGTH, reinterpret_cast<PUCHAR>(&bits), sizeof(bits), &received, 0) < 0 ||
            bits != 2048)
            throw std::runtime_error("The image requires a 2048-bit RSA public key.");
        BCRYPT_PSS_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM, static_cast<ULONG>(digest.size())};
        if (BCryptVerifySignature(key.value, &padding, digest.data(), static_cast<ULONG>(digest.size()),
                const_cast<PUCHAR>(signature.data()), static_cast<ULONG>(signature.size()), BCRYPT_PAD_PSS) < 0)
            throw std::runtime_error("The RSA firmware signature could not be verified.");
    } else {
        constexpr std::array<std::uint8_t, 10> p256{6,8,0x2a,0x86,0x48,0xce,0x3d,3,1,7};
        if (!decoded.info || std::string_view(decoded.info->Algorithm.pszObjId) != szOID_ECC_PUBLIC_KEY ||
            !std::ranges::equal(Bytes{decoded.info->Algorithm.Parameters.pbData, decoded.info->Algorithm.Parameters.cbData}, p256) ||
            decoded.info->PublicKey.cbData != 65 || decoded.info->PublicKey.cUnusedBits ||
            decoded.info->PublicKey.pbData[0] != 4)
            throw std::runtime_error("The image requires an uncompressed ECDSA P-256 public key.");
        if (!std::ranges::equal(sha256(der), keyHash))
            throw std::runtime_error("The public key does not match the image's signing-key identifier.");
        Algorithm algorithm(BCRYPT_ECDSA_P256_ALGORITHM);
        PublicKey key;
        const BCRYPT_ECCKEY_BLOB header{BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32};
        std::array<std::uint8_t, sizeof(header) + 64> blob{};
        std::memcpy(blob.data(), &header, sizeof(header));
        std::copy_n(decoded.info->PublicKey.pbData + 1, 64, blob.begin() + sizeof(header));
        if (BCryptImportKeyPair(algorithm.value, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key.value,
                                blob.data(), static_cast<ULONG>(blob.size()), 0) < 0)
            throw std::runtime_error("Windows could not import the ECDSA public key.");
        auto value = decodeEcdsaSignature(signature);
        if (BCryptVerifySignature(key.value, nullptr, digest.data(), static_cast<ULONG>(digest.size()),
                                  value.data(), static_cast<ULONG>(value.size()), 0) < 0)
            throw std::runtime_error("The ECDSA firmware signature could not be verified.");
    }
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

FirmwareImage inspectFirmware(Bytes image, Bytes trustedKey) {
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
    Bytes keyHash;
    Bytes signatureBytes;
    std::optional<ImageSignature> signature;
    readTable(image.subspan(static_cast<std::size_t>(signedBytes)), regularMagic,
        [&](std::uint16_t type, Bytes data) {
            if (type == 0x10) {
                if (!expected.empty() || data.size() != Digest{}.size())
                    throw std::runtime_error("The image has an invalid SHA-256 record.");
                expected = data;
            } else if (type == 0x01) {
                if (!keyHash.empty() || data.size() != Digest{}.size())
                    throw std::runtime_error("The image has an invalid signing-key identifier.");
                keyHash = data;
            } else if (type == 0x20 || type == 0x22) {
                if (signature || (type == 0x20 && data.size() != 256) ||
                    (type == 0x22 && (data.size() < 8 || data.size() > 80)))
                    throw std::runtime_error("The image has an invalid signature record.");
                signature = type == 0x20 ? ImageSignature::rsa2048 : ImageSignature::ecdsaP256;
                signatureBytes = data;
            }
        });
    if (expected.empty() || keyHash.empty() || !signature)
        throw std::runtime_error("The image is missing UDCAP's hash or signing metadata.");
    const auto digest = sha256(image.first(static_cast<std::size_t>(signedBytes)));
    if (!std::ranges::equal(digest, expected)) throw std::runtime_error("The firmware payload SHA-256 does not match.");
    if (!trustedKey.empty()) verifySignature(*signature, digest, keyHash, signatureBytes, trustedKey);
    return {image[20], image[21], u16(image, 22), u32(image, 24), payload, *signature, !trustedKey.empty()};
}

static std::vector<std::uint8_t> readBinary(const std::filesystem::path& path, std::size_t limit) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || !size || size > limit) throw std::runtime_error("The selected file is empty or outside the supported size.");
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> contents(static_cast<std::size_t>(size));
    if (!input.read(reinterpret_cast<char*>(contents.data()), static_cast<std::streamsize>(contents.size())) ||
        input.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("The selected file could not be read completely.");
    return contents;
}

FirmwareImage readFirmware(const std::filesystem::path& path, const std::filesystem::path& trustedKey) {
    const auto image = readBinary(path, maximumImage);
    if (trustedKey.empty()) return inspectFirmware(image);
    return inspectFirmware(image, readBinary(trustedKey, maximumKey));
}

FirmwareTarget firmwareTarget(const std::filesystem::path& path) {
    auto name = path.filename().wstring();
    for (auto& c : name) c = static_cast<wchar_t>(std::towlower(c));
    if (name == L"nrf52832_rx_app_update.bin") return FirmwareTarget::receiver;
    if (name == L"nrf52840_tx_left_app_update.bin") return FirmwareTarget::leftGlove;
    if (name == L"nrf52840_tx_right_app_update.bin") return FirmwareTarget::rightGlove;
    return FirmwareTarget::unknown;
}

FirmwareUpdateImage readUpdateImage(const std::filesystem::path& path) {
    auto contents = readBinary(path, maximumImage);
    const auto version = inspectFirmware(contents);
    const auto digest = sha256(contents);
    constexpr std::string_view left = "6f19f1242029c3468d051cca8bdbcf4aec8cd463207e43a8ac2596c3d9da3637";
    constexpr std::string_view right = "1d6103f03378997caa96333eda047f2f22c93b091ec866c17ad3c61fb9226a29";
    constexpr std::string_view receiver = "23743bcc00ffadea0c5b81ad63d27da4361f8ddadf484a11982633dbd0309b4e";
    const auto matches = [&digest](std::string_view expected) {
        constexpr std::string_view digits{"0123456789abcdef"};
        for (std::size_t i = 0; i < digest.size(); ++i)
            if (digits[digest[i] >> 4] != expected[2*i] || digits[digest[i] & 15] != expected[2*i+1]) return false;
        return true;
    };
    const auto target = matches(left) ? FirmwareTarget::leftGlove :
                        matches(right) ? FirmwareTarget::rightGlove :
                        matches(receiver) ? FirmwareTarget::receiver : FirmwareTarget::unknown;
    if (target == FirmwareTarget::unknown)
        throw std::runtime_error("Choose an original glove or receiver firmware image from UDCAP 0.1.8.7.");
    return {target, version, std::move(contents)};
}
}
