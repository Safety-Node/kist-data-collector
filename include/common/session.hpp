#pragma once

// Session directory + meta.yaml. A session is one recording run:
//
//   <output_dir>/<YYYYMMDD_HHMMSS>/
//     meta.yaml            <- written at start, summary appended at stop
//     <camera>/color.h264 color.idx.csv depth.rvl depth.idx.csv
//
// The dir name is local time (operator-facing); meta.yaml carries the
// UTC + epoch-ns start/end for machine alignment across hosts.

#include "common/stream_stats.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace kist {

struct SessionInfo {
    std::string dir;          // <output_dir>/<stamp>, created
    int64_t     started_ns;   // host CLOCK_REALTIME, epoch ns
};

// Creates <output_dir>/<YYYYMMDD_HHMMSS>/ (and parents). Empty dir on failure.
SessionInfo session_create(const std::string& output_dir);

// Writes meta.yaml: start time, DDS endpoint, camera list, format notes.
void session_write_meta(const SessionInfo& session, int domain_id,
                        const std::string& dds_config,
                        const std::vector<std::string>& cameras);

// Appends the end time + per-stream counters to meta.yaml on clean stop.
struct StreamSummary {
    std::string source;   // camera name | "unitree" | "dex3" | ...
    std::string stream;   // "color" | "depth" | "lowstate" | "hand_left" | ...
    StreamStats stats;
};
void session_finalize_meta(const SessionInfo& session,
                           const std::vector<StreamSummary>& streams);

} // namespace kist
