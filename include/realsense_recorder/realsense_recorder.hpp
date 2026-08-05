#pragma once

// Per-camera recording assembly (owns no DDS thread of its own) — the
// storage-side sibling of kist::RealsenseReceiver, but with its own
// RELIABLE-capable subscribers instead of ext's best-effort ones (see
// reliable_subscriber.hpp for why), feeding BlobRecorders that persist the
// compressed payloads verbatim:
//
//   [ReliableSubscriber] --on_frame--> [BlobRecorder] -> color.h264 + color.idx.csv
//   [ReliableSubscriber] --on_frame--> [BlobRecorder] -> depth.rvl  + depth.idx.csv
//
// One instance per camera; `name` selects the topics (rt/kist/camera/<name>/...)
// exactly like the receiver. Files land under <session_dir>/<name>/.

#include "common/blob_recorder.hpp"
#include "realsense_recorder/reliable_subscriber.hpp"
#include "realsense/h264_color_frame.hpp"
#include "realsense/rvl_depth_frame.hpp"

#include "kist_camera_frames.hpp"  // idlc-generated

#include <string>

namespace kist {

class RealsenseRecorder {
public:
    // Creates <session_dir>/<name>/ and starts both stream recorders +
    // subscribers. queue_capacity bounds each stream's in-flight frames
    // (30 fps -> capacity/30 s of disk-stall headroom). `reliable` selects
    // the reader QoS (retransmission vs plain best-effort).
    bool start(int domain_id, const std::string& name,
               const std::string& session_dir, size_t queue_capacity,
               bool reliable);

    // Stops the subscribers first (no more frames), then drains and closes
    // both recorders — every frame received before stop() is on disk after.
    void stop();

    const std::string& name() const { return name_; }

    StreamStats color_stats() const { return color_rec_.stats(); }
    StreamStats depth_stats() const { return depth_rec_.stats(); }

private:
    std::string name_;
    bool        running_ = false;

    ReliableSubscriber<kist_msgs::CompressedColorFrame, H264ColorFrame> color_sub_;
    ReliableSubscriber<kist_msgs::CompressedDepthFrame, RvlDepthFrame>  depth_sub_;

    BlobRecorder<H264ColorFrame> color_rec_;
    BlobRecorder<RvlDepthFrame>  depth_rec_;
};

} // namespace kist
