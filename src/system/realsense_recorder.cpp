#include "system/realsense_recorder.hpp"

#include "realsense_recorder/frame_maps.hpp"
#include "realsense_recorder/frame_rows.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

namespace kist {

bool RealsenseRecorder::start(int domain_id, const std::string& name,
                              const std::string& session_dir, size_t queue_capacity,
                              bool reliable) {
    if (running_) return true;
    name_ = name;

    const std::filesystem::path dir = std::filesystem::path(session_dir) / name;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::cerr << "[RealsenseRecorder] cannot create " << dir << ": " << ec.message() << "\n";
        return false;
    }

    if (!color_rec_.open((dir / "color.h264").string(), (dir / "color.idx.csv").string(),
                         kColorIdxHeader, &color_row, queue_capacity) ||
        !depth_rec_.open((dir / "depth.rvl").string(), (dir / "depth.idx.csv").string(),
                         kDepthIdxHeader, &depth_row, queue_capacity)) {
        std::cerr << "[RealsenseRecorder] cannot open output files under " << dir << "\n";
        color_rec_.close();
        depth_rec_.close();
        return false;
    }

    if (!color_sub_.start(domain_id, camera_color_topic(name), reliable, &map_color,
                          [this](const H264ColorFrame& f) { color_rec_.push(f); }) ||
        !depth_sub_.start(domain_id, camera_depth_topic(name), reliable, &map_depth,
                          [this](const RvlDepthFrame& f) { depth_rec_.push(f); })) {
        stop();
        return false;
    }

    running_ = true;
    return true;
}

void RealsenseRecorder::stop() {
    // Grace before closing the readers: a frame under RELIABLE
    // retransmission at this instant needs up to ~1 heartbeat cycle
    // (~100 ms) to land; closing immediately would abandon it. Frames
    // arriving during the grace are recorded normally.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    color_sub_.stop();
    depth_sub_.stop();
    color_rec_.close();
    depth_rec_.close();
    running_ = false;
}

} // namespace kist
