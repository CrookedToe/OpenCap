#include "bridge.hpp"

#include <windows.h>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace opencap;

static void require(bool result, const char* description) {
    if (!result) throw std::runtime_error(description);
}

int main() {
    try {
        const auto name = L"Local\\OpenCap.Checks." + std::to_wstring(GetCurrentProcessId());
        Bus writer(name), reader(name);
        require(writer.valid() && reader.valid(), "Create shared session");
        Snapshot sample;
        sample.heartbeat = 2000;
        sample.hands[0] = demoHand(Side::left, 2000);
        require(writer.publish(sample), "Publish a snapshot");
        const auto seen = reader.read(100);
        require(seen && isLive(*seen, Side::left, 2200), "Snapshot crosses the session boundary");
        require(!isLive(*seen, Side::right, 2200), "Inactive hand stays disconnected");
        require(!isLive(*seen, Side::left, 2501), "Expired input is released");
        require(!isLive(*seen, Side::left, 1999), "Future timestamps are rejected");

        for (int i = 0; i < 32; ++i)
            require(reader.send({Side::right, Action::channel, i}), "Queue accepts commands");
        require(!reader.send({Side::right, Action::channel, 32}), "Full queue preserves prior commands");
        for (int i = 0; i < 32; ++i) {
            const auto command = writer.next();
            require(command && command->side == Side::right && command->value == i, "Queue preserves order");
        }
        require(!writer.next(), "Consumed commands are not repeated");
        require(reader.send({Side::left, Action::toggleStick}), "Queue can wrap");
        require(writer.next()->action == Action::toggleStick, "Wrapped command is readable");

        const auto point = rotate(eulerDegrees({0, 0, 90}), {1, 0, 0});
        require(std::abs(point.x) < 1e-5f && std::abs(point.y - 1) < 1e-5f, "Rotation is right-handed");
        require(normalize({0, 0, 0, 0}).w == 1.0f, "Invalid quaternion resets to identity");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
