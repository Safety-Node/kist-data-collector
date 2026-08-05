#include "system/realsense_recorder.hpp"

#include <cinttypes>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

namespace kist {

namespace {

// Index rows: everything a reader needs to slice + interpret one frame
// without decoding its neighbors. stamp_ns = Tx capture clock (epoch ns),
// recv_ns = this host's arrival clock (epoch ns) — recv_ns is what aligns
// camera frames with the other streams recorded on this machine.

std::string color_row(const H264ColorFrame& f, int64_t recv_ns) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%" PRIu64 ",%" PRId64 ",%" PRId64 ",%d,%d,%d",
                  f.sequence, f.stamp_ns, recv_ns, f.width, f.height,
                  f.is_keyframe ? 1 : 0);
    return buf;
}

std::string depth_row(const RvlDepthFrame& f, int64_t recv_ns) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%" PRIu64 ",%" PRId64 ",%" PRId64 ",%d,%d,%.6g",
                  f.sequence, f.stamp_ns, recv_ns, f.width, f.height,
                  double(f.depth_scale));
    return buf;
}

constexpr const char* kColorIdxHeader =
    "seq,stamp_ns,recv_ns,width,height,is_keyframe,offset,size";
constexpr const char* kDepthIdxHeader =
    "seq,stamp_ns,recv_ns,width,height,depth_scale,offset,size";

// Wire message -> frame struct, the same mapping ext's subscribers do.

H264ColorFrame map_color(const kist_msgs::CompressedColorFrame& msg) {
    H264ColorFrame f;
    f.width       = int(msg.width());
    f.height      = int(msg.height());
    f.sequence    = msg.seq();
    f.stamp_ns    = msg.stamp_ns();
    f.is_keyframe = msg.is_keyframe();
    f.frame_id    = msg.frame_id();
    f.data        = msg.data();
    return f;
}

RvlDepthFrame map_depth(const kist_msgs::CompressedDepthFrame& msg) {
    RvlDepthFrame f;
    f.width       = int(msg.width());
    f.height      = int(msg.height());
    f.sequence    = msg.seq();
    f.stamp_ns    = msg.stamp_ns();
    f.depth_scale = msg.depth_scale();
    f.frame_id    = msg.frame_id();
    f.data        = msg.data();
    return f;
}

} // namespace

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
