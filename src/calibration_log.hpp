#pragma once

#include "sensor_calibration.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace opencap {
struct CalibrationRecord {
    std::int64_t timeUs{};
    const char* event{};
    const char* hand{};
    std::string serial;
    bool automatic{};
    int guideId{};
    std::optional<SensorFrame> frame;
    std::optional<SensorRange> range;
    std::string detail;
};

class CalibrationLog {
public:
    ~CalibrationLog() { stop(); }
    void start(const std::filesystem::path& folder);
    void stop();
    bool active() const { return active_.load(); }
    void record(CalibrationRecord row);
    std::string takeError();
private:
    void write(std::ofstream file);
    std::atomic<bool> active_{false};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<CalibrationRecord> pending_;
    std::string error_;
    bool overflowed_{};
    std::thread writer_;
};
}
