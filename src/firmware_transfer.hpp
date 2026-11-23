#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

namespace opencap {
using FirmwareWrite = std::function<void(std::span<const std::uint8_t>)>;
using FirmwareRead = std::function<std::size_t(std::span<std::uint8_t>)>;
using FirmwareProgress = std::function<void(unsigned)>;
enum class FirmwareTransport { usb, wireless };
char wiredGloveSide(std::string_view line);

void sendFirmware(std::span<const std::uint8_t> image, const FirmwareWrite& write,
                  const FirmwareRead& read, const FirmwareProgress& progress,
                  FirmwareTransport transport = FirmwareTransport::usb);
}
