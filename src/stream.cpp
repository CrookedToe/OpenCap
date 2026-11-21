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
constexpr std::array fingers{"Thumb", "Index", "Middle", "Ring", "Little"};
constexpr std::array joints{"Proximal", "Intermediate", "Distal"};

void word(Packet& packet, std::string_view value) {
    packet.insert(packet.end(), value.begin(), value.end());
    do { packet.push_back(0); } while (packet.size() % 4);
}

void number(Packet& packet, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) packet.push_back(static_cast<char>(value >> shift));
}

void scalar(Packet& packet, float value) { number(packet, std::bit_cast<std::uint32_t>(value)); }

Packet bundle() {
    Packet result;
    word(result, "#bundle");
    number(result, 0);
    number(result, 1);
    return result;
}

void append(Packet& container, const Packet& message) {
    number(container, static_cast<std::uint32_t>(message.size()));
    container.insert(container.end(), message.begin(), message.end());
}

Packet status(bool active) {
    Packet packet;
    word(packet, "/avatar/parameters/HandTrack");
    word(packet, active ? ",T" : ",F");
    return packet;
}

Packet vmcHand(const Hand& hand, std::size_t side) {
    auto packet = bundle();
    for (std::size_t f = 0; f < fingers.size(); ++f) {
        for (std::size_t j = 0; j < joints.size(); ++j) {
            Packet message;
            word(message, "/VMC/Ext/Bone/Pos");
            word(message, ",sfffffff");
            word(message, std::string(sides[side]) + fingers[f] + joints[j]);
            scalar(message, 0);
            scalar(message, 0);
            scalar(message, 0);
            const auto q = normalize(hand.joints[3*f+j].rotation);
            for (const float component : {q.x, q.y, q.z, q.w}) scalar(message, component);
            append(packet, message);
        }
    }
    return packet;
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

    bool started{};
    SOCKET socket{INVALID_SOCKET};
    sockaddr_in vmc{}, osc{};
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
    vmc.sin_port = htons(static_cast<u_short>(settings.vmcPort));
    osc.sin_port = htons(static_cast<u_short>(settings.oscPort));
    impl_->vmc = vmc;
    impl_->osc = osc;
    impl_->oscWasLive = false;
    impl_->lastOscRefresh = 0;
    return true;
}

void Stream::send(const Snapshot& snapshot) {
    if (!impl_->vmc.sin_port && !impl_->osc.sin_port) return;
    bool active = false;
    const bool refresh = !impl_->oscWasLive || snapshot.heartbeat - impl_->lastOscRefresh >= 1000;
    for (std::size_t i = 0; i < handCount; ++i) {
        const auto& hand = snapshot.hands[i];
        if (!hand.calibrated || !isLive(snapshot, static_cast<Side>(i), snapshot.heartbeat)) continue;
        active = true;
        if (impl_->vmc.sin_port) impl_->transmit(vmcHand(hand, i), impl_->vmc);
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
    if (impl_->osc.sin_port && (impl_->oscWasLive != active || (active && refresh))) {
        impl_->transmit(status(active), impl_->osc);
        impl_->lastOscRefresh = snapshot.heartbeat;
    }
    impl_->oscWasLive = active;
}
}
