#include "uwb_recorder/uwb_recorder.hpp"

#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <iostream>

namespace kist {

namespace {

constexpr const char* kUwbHeader = "recv_ns,stamp_ns,x,y,z";

std::string uwb_row(const UwbPosition& fix, int64_t recv_ns) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%" PRId64 ",%" PRId64 ",%.7g,%.7g,%.7g",
                  recv_ns, fix.stamp_ns, double(fix.x), double(fix.y), double(fix.z));
    return buf;
}

} // namespace

bool UwbRecorder::start(int domain_id, const std::string& network_interface,
                        const std::string& session_dir, size_t queue_capacity) {
    if (running_) return true;

    const auto csv_path = std::filesystem::path(session_dir) / "uwb.csv";
    if (!rec_.open(csv_path.string(), kUwbHeader, &uwb_row, queue_capacity)) {
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
