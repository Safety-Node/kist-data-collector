#pragma once

// Wire message -> frame struct, the same mapping ext's subscribers do —
// kept here (not in ext) because the RELIABLE path takes the typed message
// straight off the wire (reliable_subscriber.hpp) instead of going through
// ext's best-effort receiver.

#include "realsense/h264_color_frame.hpp"
#include "realsense/rvl_depth_frame.hpp"

#include "kist_camera_frames.hpp"  // idlc-generated

namespace kist {

inline H264ColorFrame map_color(const kist_msgs::CompressedColorFrame& msg) {
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

inline RvlDepthFrame map_depth(const kist_msgs::CompressedDepthFrame& msg) {
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

} // namespace kist
