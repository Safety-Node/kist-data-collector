#include "system/uwb_recorder.hpp"

#include "uwb_recorder/uwb_rows.hpp"

#include <filesystem>
#include <iostream>

namespace kist {

bool UwbRecorder::start(int domain_id, const std::string& network_interface,
                        const std::string& session_dir, size_t queue_capacity) {
    if (running_) return true;

    const auto csv_path = std::filesystem::path(session_dir) / "uwb.csv";
    if (!rec_.open(csv_path.string(), kUwbCsvHeader, &uwb_row, queue_capacity)) {
        std::cerr << "[UwbRecorder] cannot open " << csv_path << "\n";
        return false;
    }

    // Hook before start(): no window where a delivered fix has no sink.
    sub_.set_on_position([this](const UwbPosition& fix) { rec_.push(fix); });

    if (!sub_.start(domain_id, network_interface)) {
        rec_.close();
        return false;
    }

    running_ = true;
    return true;
}

void UwbRecorder::stop() {
    sub_.stop();
    rec_.close();
    running_ = false;
}

} // namespace kist
