#pragma once

#include "model.hpp"
#include "udcap_preferences.hpp"

#include <array>
#include <filesystem>
#include <memory>

namespace opencap {
class Receiver {
public:
    Receiver(const std::filesystem::path& preferences, std::array<ReceiverPreferences, handCount> defaults,
             std::uint64_t importRevision);
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    std::array<Hand, handCount> read() const;
    bool submit(const Command& command);
    void importPreferences(std::array<ReceiverPreferences, handCount> defaults, std::uint64_t importRevision);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
