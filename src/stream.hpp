#pragma once

#include "model.hpp"

#include <memory>
#include <string>

namespace opencap {
struct StreamSettings {
    std::string address{"127.0.0.1"};
    unsigned vmcPort{};
    unsigned oscPort{};
};

class Stream {
public:
    Stream();
    ~Stream();
    bool configure(const StreamSettings& settings);
    void send(const Snapshot& snapshot);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
