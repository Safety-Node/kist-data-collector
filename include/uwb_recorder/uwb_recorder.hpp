#pragma once

// UWB recording assembly — kist-ext-sensor-io's UwbSubscriber (SDK path,
// like lowstate) feeding a RowRecorder:
//
//   [DDS rt/kist/uwb/pose] --on_position--> [RowRecorder] -> uwb.csv
//
// Rows: recv_ns, stamp_ns, x, y, z (UWB local frame, meters). The wire
// carries no sequence number, and UWB silence is a legitimate state (tag
// out of anchor range; the transmitter drops invalid fixes) — so time gaps
// between rows mean "no fix", not loss, and wire_gaps stays 0 by design.

#include "common/row_recorder.hpp"
#include "uwb/uwb_position.hpp"
#include "uwb/receiver/uwb_subscriber.hpp"

#include <string>

namespace kist {

class UwbRecorder {
public:
    // Opens <session_dir>/uwb.csv and starts the subscriber + writer.
    bool start(int domain_id, const std::string& network_interface,
               const std::string& session_dir, size_t queue_capacity);

    // Stops the subscriber first (no more fixes), then drains and closes
    // the writer — every fix received before stop() is on disk after.
    void stop();

    StreamStats stats() const { return rec_.stats(); }

private:
    UwbSubscriber sub_;
    RowRecorder<UwbPosition> rec_;
    bool running_ = false;
};

} // namespace kist
