#pragma once

#include "model.hpp"

#include <memory>
#include <optional>
#include <string>

namespace opencap {
Millis now();

class Bus {
public:
    explicit Bus(const std::wstring& name = L"Local\\OpenCap.Session.v2");
    ~Bus();
    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    bool valid() const;
    bool publish(const Snapshot& snapshot);
    std::optional<Snapshot> read(unsigned waitMs = 0) const;
    bool send(const Command& command);
    std::optional<Command> next();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
