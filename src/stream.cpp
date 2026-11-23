#include <winsock2.h>
#include <ws2tcpip.h>
#include "stream.hpp"
#include "osc_compat.hpp"

#include <bit>
#include <string_view>
#include <vector>

namespace opencap {
namespace {
using Packet = std::vector<char>;
constexpr std::array sides{"Left", "Right"};
constexpr std::array fingers{"Index", "Middle", "Ring", "Little", "Thumb"};
constexpr std::array joints{"Proximal", "Intermediate", "Distal"};
constexpr std::array neutralBones{"Hips", "UpperLeg", "LeftUpperLeg", "RightUpperLeg", "LeftLowerLeg",
    "RightLowerLeg", "Foot", "Spine", "Chest", "Neck", "Head", "Shoulder", "LeftShoulder", "RightShoulder",
    "UpperArm", "LeftUpperArm", "RightUpperArm", "LowerArm", "LeftLowerArm", "RightLowerArm",
    "Hand", "LeftHand", "RightHand", "Eye", "LeftEye"};

void word(Packet& packet, std::string_view value) {
    packet.insert(packet.end(), value.begin(), value.end());
    do { packet.push_back(0); } while (packet.size() % 4);
}

void number(Packet& packet, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) packet.push_back(static_cast<char>(value >> shift));
}

void scalar(Packet& packet, float value) { number(packet, std::bit_cast<std::uint32_t>(value)); }

Packet status(bool active) {
    Packet packet;
    word(packet, "/avatar/parameters/HandTrack");
    word(packet, active ? ",T" : ",F");
    return packet;
}

Packet vmcBone(std::string_view name, Quaternion rotation) {
    Packet message;
    word(message, "/VMC/Ext/Bone/Pos");
    word(message, ",sfffffff");
    word(message, name);
    for (const auto value : {0.0f, 0.0f, 0.0f, rotation.x, rotation.y, rotation.z, rotation.w})
        scalar(message, value);
    return message;
}

}

struct Stream::Impl {
    Impl() {
        WSADATA data{};
        started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        if (!started) return;
        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket != INVALID_SOCKET) {
            u_long nonblocking = 1;
            ioctlsocket(socket, FIONBIO, &nonblocking);
        }
    }

    ~Impl() {
        if (socket != INVALID_SOCKET) closesocket(socket);
        if (started) WSACleanup();
    }

    void transmit(const Packet& message, const sockaddr_in& endpoint) const {
        if (socket == INVALID_SOCKET || !endpoint.sin_port) return;
        sendto(socket, message.data(), static_cast<int>(message.size()), 0,
               reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint));
    }

    void sendVmc(const Snapshot& snapshot, const std::array<bool, handCount>& live) const {
        if (!vmc.sin_port || (!live[0] && !live[1] && !vmcWasLive)) return;
        auto samples = snapshot.hands;
        for (std::size_t side = 0; side < handCount; ++side) {
            if (!live[side]) samples[side] = {};
            if (!snapshot.settings[side].joystick) {
                samples[side].stickX = samples[side].stickY = 0;
                samples[side].buttons &= ~bit(Button::stick);
            }
        }
        for (const auto name : neutralBones) transmit(vmcBone(name, {}), vmc);
        for (std::size_t side = 0; side < handCount; ++side) {
            const auto rotations = vmcFingerRotations(samples[side], static_cast<Side>(side));
            const std::string prefix = sides[side];
            for (std::size_t f = 0; f < fingers.size(); ++f) {
                for (std::size_t j = 0; j < joints.size(); ++j)
                    transmit(vmcBone(prefix + fingers[f] + joints[j], rotations[3*f+j]), vmc);
            }
        }
        for (std::size_t side = 0; side < handCount; ++side) {
            for (const auto& input : vmcControllerInputs(samples[side])) {
                Packet message;
                word(message, "/VMC/Ext/Blend/Val");
                word(message, ",sf");
                word(message, std::string(sides[side]) + std::string(input.control));
                scalar(message, input.value);
                transmit(message, vmc);
            }
        }
        Packet apply;
        word(apply, "/VMC/Ext/Blend/Apply");
        word(apply, ",");
        transmit(apply, vmc);
    }

    bool started{};
    SOCKET socket{INVALID_SOCKET};
    sockaddr_in vmc{}, osc{};
    bool vmcWasLive{};
    bool oscWasLive{};
    Millis lastOscRefresh{};
    std::array<std::array<unsigned, avatarParameterCount>, handCount> previousBits{};
};

Stream::Stream() : impl_(std::make_unique<Impl>()) {}
Stream::~Stream() = default;

bool Stream::configure(const StreamSettings& settings) {
    if (settings.vmcPort > 65535 || settings.oscPort > 65535) return false;
    sockaddr_in vmc{}, osc{};
    vmc.sin_family = osc.sin_family = AF_INET;
    if (InetPtonA(AF_INET, settings.address.c_str(), &vmc.sin_addr) != 1 ||
        InetPtonA(AF_INET, settings.oscAddress.c_str(), &osc.sin_addr) != 1) return false;
    if ((settings.vmcPort || settings.oscPort) && impl_->socket == INVALID_SOCKET) return false;
    if (impl_->oscWasLive) impl_->transmit(status(false), impl_->osc);
    if (impl_->vmcWasLive) impl_->sendVmc({}, {});
    vmc.sin_port = htons(static_cast<u_short>(settings.vmcPort));
    osc.sin_port = htons(static_cast<u_short>(settings.oscPort));
    impl_->vmc = vmc;
    impl_->osc = osc;
    impl_->vmcWasLive = false;
    impl_->oscWasLive = false;
    impl_->lastOscRefresh = 0;
    return true;
}

void Stream::send(const Snapshot& snapshot) {
    if (!impl_->vmc.sin_port && !impl_->osc.sin_port) return;
    bool active = false;
    std::array<bool, handCount> live{};
    const bool refresh = !impl_->oscWasLive || snapshot.heartbeat - impl_->lastOscRefresh >= 1000;
    for (std::size_t i = 0; i < handCount; ++i) {
        const auto& hand = snapshot.hands[i];
        if (!hand.calibrated || !isLive(snapshot, static_cast<Side>(i), snapshot.heartbeat)) continue;
        live[i] = true;
        active = true;
        if (!impl_->osc.sin_port) continue;
        const auto parameters = avatarParameters(snapshot, static_cast<Side>(i));
        for (std::size_t control = 0; control < parameters.size(); ++control) {
            const auto& parameter = parameters[control];
            const auto previous = impl_->previousBits[i][control];
            impl_->previousBits[i][control] = parameter.bits;
            for (unsigned mask = 1; mask < 16; mask *= 2) {
                if (!refresh && !((previous ^ parameter.bits) & mask)) continue;
                Packet message;
                word(message, std::string("/avatar/parameters/") + sides[i]
                    + std::string(parameter.control) + std::to_string(mask));
                word(message, parameter.bits & mask ? ",T" : ",F");
                impl_->transmit(message, impl_->osc);
            }
        }
    }
    impl_->sendVmc(snapshot, live);
    impl_->vmcWasLive = active;
    if (impl_->osc.sin_port && (impl_->oscWasLive != active || (active && refresh))) {
        impl_->transmit(status(active), impl_->osc);
        impl_->lastOscRefresh = snapshot.heartbeat;
    }
    impl_->oscWasLive = active;
}
}
