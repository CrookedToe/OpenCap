#include "bridge.hpp"

#include <windows.h>
#include <array>

namespace opencap {
namespace {
constexpr std::uint32_t magic = 0x4f434150;
constexpr std::size_t queueSize = 32;

struct SharedState {
    std::uint32_t marker{};
    Snapshot snapshot{};
    TrackingSnapshot tracking{};
    std::size_t readPosition{};
    std::size_t count{};
    std::array<Command, queueSize> commands{};
};

class Lock {
public:
    Lock(HANDLE mutex, unsigned waitMs) : mutex_(mutex) {
        if (mutex) {
            const auto result = WaitForSingleObject(mutex, waitMs);
            acquired_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
            abandoned_ = result == WAIT_ABANDONED;
        }
    }
    ~Lock() { if (acquired_) ReleaseMutex(mutex_); }
    explicit operator bool() const { return acquired_; }
    bool abandoned() const { return abandoned_; }
private:
    HANDLE mutex_{};
    bool acquired_{};
    bool abandoned_{};
};
}

struct Bus::Impl {
    HANDLE mutex{};
    HANDLE mapping{};
    SharedState* state{};

    ~Impl() {
        if (state) UnmapViewOfFile(state);
        if (mapping) CloseHandle(mapping);
        if (mutex) CloseHandle(mutex);
    }

    bool validate(const Lock& lock) const {
        if (!state || !lock) return false;
        if (lock.abandoned() || state->marker != magic) {
            *state = SharedState{};
            state->marker = magic;
        }
        return true;
    }
};

Millis now() { return GetTickCount64(); }

std::wstring widen(std::string_view value) {
    if (value.empty()) return {};
    const auto n = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), n);
    return out;
}

Bus::Bus(const std::wstring& name) : impl_(std::make_unique<Impl>()) {
    impl_->mutex = CreateMutexW(nullptr, FALSE, (name + L".lock").c_str());
    const Lock lock(impl_->mutex, 1000);
    if (!lock) return;
    impl_->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                       sizeof(SharedState), name.c_str());
    if (!impl_->mapping) return;
    impl_->state = static_cast<SharedState*>(MapViewOfFile(impl_->mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                                          sizeof(SharedState)));
    impl_->validate(lock);
}

Bus::~Bus() = default;

bool Bus::valid() const { return impl_->state != nullptr; }

bool Bus::reset() {
    const Lock lock(impl_->mutex, 100);
    if (!impl_->validate(lock)) return false;
    *impl_->state = SharedState{};
    impl_->state->marker = magic;
    return true;
}

bool Bus::publish(const Snapshot& snapshot) {
    const Lock lock(impl_->mutex, 5);
    if (!impl_->validate(lock)) return false;
    impl_->state->snapshot = snapshot;
    return true;
}

std::optional<Snapshot> Bus::read(unsigned waitMs) const {
    const Lock lock(impl_->mutex, waitMs);
    if (!impl_->validate(lock)) return std::nullopt;
    return impl_->state->snapshot;
}

bool Bus::publishTracking(const TrackingSnapshot& snapshot) {
    const Lock lock(impl_->mutex, 0);
    if (!impl_->validate(lock)) return false;
    impl_->state->tracking = snapshot;
    return true;
}

std::optional<TrackingSnapshot> Bus::readTracking() const {
    const Lock lock(impl_->mutex, 0);
    if (!impl_->validate(lock)) return std::nullopt;
    return impl_->state->tracking;
}

bool Bus::send(const Command& command) {
    return send(std::span{&command, 1});
}

bool Bus::send(std::span<const Command> commands) {
    const Lock lock(impl_->mutex, 1);
    if (!impl_->validate(lock)) return false;
    auto& state = *impl_->state;
    if (commands.size() > queueSize - state.count) return false;
    for (const auto& command : commands) {
        state.commands[(state.readPosition + state.count) % queueSize] = command;
        ++state.count;
    }
    return true;
}

std::optional<Command> Bus::next() {
    const Lock lock(impl_->mutex, 1);
    if (!impl_->validate(lock)) return std::nullopt;
    auto& state = *impl_->state;
    if (state.count == 0) return std::nullopt;
    const auto command = state.commands[state.readPosition];
    state.readPosition = (state.readPosition + 1) % queueSize;
    --state.count;
    return command;
}
}
