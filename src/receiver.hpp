#pragma once

#include "model.hpp"

#include <array>
#include <filesystem>
#include <memory>

namespace opencap {
class Receiver {
public:
    explicit Receiver(const std::filesystem::path& preferences);
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    std::array<Hand, handCount> read() const;
    bool submit(const Command& command);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
