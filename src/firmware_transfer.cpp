#include "firmware_transfer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <vector>

namespace opencap {
namespace {
using Bytes = std::span<const std::uint8_t>;
using Clock = std::chrono::steady_clock;
constexpr std::uint8_t wiredUpdate = 3, radioUpdate = 0xde;
constexpr std::size_t wiredBlockSize = 400, radioBlockSize = 160;

std::uint16_t modbusCrc(Bytes contents) {
    std::uint16_t crc = 0xffff;
    for (const auto byte : contents) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = static_cast<std::uint16_t>((crc >> 1) ^ ((crc & 1) ? 0xa001 : 0));
    }
    return crc;
}

std::vector<std::uint8_t> packet(Bytes chunk, std::size_t fileSize, std::size_t blocks, std::size_t index, bool wireless) {
    const auto length = chunk.size() + 10;
    std::vector<std::uint8_t> result;
    if (wireless) result = {0x55, 0xaa, 1, radioUpdate, 0};
    else result = {0xa5, static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length), wiredUpdate, 0};
    const std::array metadata{
        static_cast<std::uint8_t>(fileSize >> 24), static_cast<std::uint8_t>(fileSize >> 16),
        static_cast<std::uint8_t>(fileSize >> 8), static_cast<std::uint8_t>(fileSize),
        static_cast<std::uint8_t>(blocks >> 8), static_cast<std::uint8_t>(blocks),
        static_cast<std::uint8_t>(index >> 8), static_cast<std::uint8_t>(index)};
    result.insert(result.end(), metadata.begin(), metadata.end());
    result.insert(result.end(), chunk.begin(), chunk.end());
    if (wireless) result.resize(5 + metadata.size() + radioBlockSize, 0);
    const auto crc = modbusCrc(Bytes(result).subspan(3));
    result.insert(result.end(), {static_cast<std::uint8_t>(crc), static_cast<std::uint8_t>(crc >> 8), 0x5a});
    return result;
}

bool takeAcknowledgment(std::vector<std::uint8_t>& buffer, std::size_t index, bool wireless) {
    while (!buffer.empty()) {
        const auto at = std::ranges::find_if(buffer, [](auto byte) { return byte == 0xa5 || byte == 0xa6; });
        buffer.erase(buffer.begin(), at);
        if (buffer.size() < 4) return false;
        const std::size_t length = (buffer[1] << 8) | buffer[2];
        const bool updateReply = buffer[3] == wiredUpdate || (wireless && buffer[3] == radioUpdate);
        if (length < 2 || length > 32 || !updateReply) {
            buffer.erase(buffer.begin());
            continue;
        }
        const auto total = length + 6;
        if (buffer.size() < total) return false;
        const auto crc = modbusCrc(Bytes(buffer).subspan(3, length));
        const auto observed = static_cast<std::uint16_t>(buffer[3 + length] | (buffer[4 + length] << 8));
        const auto reversed = static_cast<std::uint16_t>((observed << 8) | (observed >> 8));
        if ((crc != observed && crc != reversed) || buffer[total - 1] != (buffer[0] == 0xa6 ? 0x6a : 0x5a)) {
            buffer.erase(buffer.begin());
            continue;
        }
        const auto result = buffer[4];
        // Legacy radio replies contain just the low byte of the block index.
        const bool matching = length >= 4 ? static_cast<std::size_t>(buffer[5] | (buffer[6] << 8)) == index
            : !wireless || (length == 3 && buffer[5] == (index & 0xff));
        buffer.erase(buffer.begin(), buffer.begin() + total);
        if (result == 4) throw std::runtime_error("The device rejected the firmware block.");
        if (result == 1 && matching) return true;
    }
    return false;
}
}

char wiredGloveSide(std::string_view line) {
    if (!line.starts_with("channel=")) return 0;
    for (unsigned field = 0; field < 3; ++field) {
        const auto separator = line.find(',');
        if (separator == line.npos) return 0;
        line.remove_prefix(separator + 1);
    }
    line = line.substr(0, line.find_first_of(",\r"));
    const auto separator = line.find('=');
    if (separator == line.npos) return 0;
    const auto serial = line.substr(separator + 1);
    if (serial.size() < 6 || serial.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != serial.npos) return 0;
    const auto side = serial[serial.size() - 2];
    return side == 'L' || side == 'R' ? side : 0;
}

void sendFirmware(Bytes image, const FirmwareWrite& write, const FirmwareRead& read,
                  const FirmwareProgress& progress, FirmwareTransport transport) {
    if (image.empty() || image.size() > 2 * 1024 * 1024) throw std::runtime_error("Invalid firmware transfer size.");
    const bool wireless = transport == FirmwareTransport::wireless;
    const auto blockSize = wireless ? radioBlockSize : wiredBlockSize;
    const auto blocks = (image.size() + blockSize - 1) / blockSize;
    std::array<std::uint8_t, 512> input{};
    std::vector<std::uint8_t> reply;
    for (std::size_t index = 0; index < blocks; ++index) {
        const auto chunk = image.subspan(index * blockSize, std::min(blockSize, image.size() - index * blockSize));
        write(packet(chunk, image.size(), blocks, index, wireless));
        const auto deadline = Clock::now() + std::chrono::seconds(6);
        while (!takeAcknowledgment(reply, index, wireless)) {
            if (Clock::now() >= deadline)
                throw std::runtime_error("The device stopped acknowledging firmware blocks. Reconnect it before retrying.");
            const auto count = read(input);
            if (count > input.size()) throw std::runtime_error("Invalid serial read length.");
            reply.insert(reply.end(), input.begin(), input.begin() + count);
        }
        progress(static_cast<unsigned>((index + 1) * 100 / blocks));
    }
}
}
