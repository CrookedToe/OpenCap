#pragma once

#include "model.hpp"

#include <memory>
#include <optional>
#include <string>

namespace opencap {
Millis now();
inline constexpr wchar_t sessionName[] = L"Local\\OpenCap.Session.v8";

class Bus {
public:
    explicit Bus(const std::wstring& name = sessionName);
    ~Bus();
    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    bool valid() const;
    bool reset();
    bool publish(const Snapshot& snapshot);
    std::optional<Snapshot> read(unsigned waitMs = 0) const;
    bool publishTracking(const TrackingSnapshot& snapshot);
    std::optional<TrackingSnapshot> readTracking() const;
    bool send(const Command& command);
    std::optional<Command> next();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
