#include "calibration_log.hpp"

#include <nlohmann/json.hpp>
#include <windows.h>
#include <utility>
#include <vector>

namespace opencap {
using Json = nlohmann::json;

void CalibrationLog::start(const std::filesystem::path& folder) {
    stop();
    std::filesystem::create_directories(folder);
    const auto time = sensorTimeUs();
    const auto base = "calibration-" + std::to_string(time) + "-" + std::to_string(GetCurrentProcessId());
    auto path = folder / (base + ".jsonl");
    for (unsigned i = 1; std::filesystem::exists(path); ++i)
        path = folder / (base + "-" + std::to_string(i) + ".jsonl");
    std::ofstream file(path);
    file << Json{{"schema", 1}, {"event", "recording_start"}, {"utc_us", time}}.dump() << std::endl;
    if (!file) throw std::runtime_error("Could not create the calibration recording.");
    std::lock_guard lock(mutex_);
    error_.clear();
    overflowed_ = false;
    writer_ = std::thread(&CalibrationLog::write, this, std::move(file));
    active_ = true;
}

void CalibrationLog::stop() {
    {
        std::lock_guard lock(mutex_);
        active_ = false;
    }
    changed_.notify_one();
    if (writer_.joinable()) writer_.join();
}

std::string CalibrationLog::takeError() {
    std::lock_guard lock(mutex_);
    return std::exchange(error_, {});
}

void CalibrationLog::record(CalibrationRecord row) {
    std::lock_guard lock(mutex_);
    if (!active_) return;
    if (pending_.size() >= 8192) {
        active_ = false;
        error_ = "Calibration recording stopped because the disk writer fell behind.";
        overflowed_ = true;
        changed_.notify_one();
        return;
    }
    pending_.push_back(std::move(row));
    if (pending_.size() == 128) changed_.notify_one();
}

void CalibrationLog::write(std::ofstream file) {
    try {
        file.exceptions(std::ios::badbit | std::ios::failbit);
        for (;;) {
            std::deque<CalibrationRecord> batch;
            {
                std::unique_lock lock(mutex_);
                changed_.wait_for(lock, std::chrono::milliseconds(250), [&] { return !active_ || pending_.size() >= 128; });
                if (!active_ && pending_.empty()) break;
                batch.swap(pending_);
            }
            for (const auto& row : batch) {
                Json entry{{"utc_us", row.timeUs}, {"event", row.event}, {"hand", row.hand},
                           {"serial", row.serial}, {"automatic", row.automatic}};
                if (row.guideId) entry["guide_id"] = row.guideId;
                if (row.frame) {
                    const auto& f = *row.frame;
                    entry["sample_utc_us"] = f.timeUs;
                    entry["raw"] = std::vector<std::int16_t>(f.values.begin(), f.values.begin() + f.count);
                }
                if (row.range) entry["range"] = *row.range;
                if (!row.detail.empty()) entry["detail"] = row.detail;
                file << entry.dump() << '\n';
            }
            file.flush();
        }
        file << Json{{"event", "recording_stop"}, {"utc_us", sensorTimeUs()}, {"overflow", overflowed_}}.dump() << std::endl;
    } catch (const std::exception&) {
        std::lock_guard lock(mutex_);
        active_ = false;
        pending_.clear();
        error_ = "Could not write the calibration recording. Check the log folder and available disk space.";
    }
}
}
