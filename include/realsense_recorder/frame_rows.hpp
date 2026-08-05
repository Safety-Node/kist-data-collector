#pragma once

// Camera index-CSV schema — one row per recorded frame: everything a reader
// needs to slice + interpret one frame without decoding its neighbors.
// stamp_ns = Tx capture clock (epoch ns), recv_ns = this host's arrival
// clock (epoch ns) — recv_ns is what aligns camera frames with the other
// streams recorded on this machine. (BlobRecorder appends ,offset,size.)

#include "realsense/h264_color_frame.hpp"
#include "realsense/rvl_depth_frame.hpp"

#include <cinttypes>
#include <cstdio>
#include <string>

namespace kist {

constexpr const char* kColorIdxHeader =
    "seq,stamp_ns,recv_ns,width,height,is_keyframe,offset,size";
constexpr const char* kDepthIdxHeader =
    "seq,stamp_ns,recv_ns,width,height,depth_scale,offset,size";

inline std::string color_row(const H264ColorFrame& f, int64_t recv_ns) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%" PRIu64 ",%" PRId64 ",%" PRId64 ",%d,%d,%d",
                  f.sequence, f.stamp_ns, recv_ns, f.width, f.height,
                  f.is_keyframe ? 1 : 0);
    return buf;
}

inline std::string depth_row(const RvlDepthFrame& f, int64_t recv_ns) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%" PRIu64 ",%" PRId64 ",%" PRId64 ",%d,%d,%.6g",
                  f.sequence, f.stamp_ns, recv_ns, f.width, f.height,
                  double(f.depth_scale));
    return buf;
}

} // namespace kist
