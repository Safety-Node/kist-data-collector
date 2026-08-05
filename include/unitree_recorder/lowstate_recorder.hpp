#pragma once

// Robot-state recording assembly — the robot-state sibling of RealsenseRecorder:
// subscribes the G1's rt/lowstate (unitree_hg LowState_, ~1 kHz) and rows
// every message into <session_dir>/lowstate.csv via RowRecorder:
//
//   [DDS rt/lowstate] --callback--> [RowRecorder] -> lowstate.csv
//
// Columns: recv_ns (this host's arrival clock, the cross-stream alignment
// column), tick + modes, IMU (quaternion/gyro/accel/rpy/temp), and per-motor
// q/dq/ddq/tau_est for all 35 slots. One instance per robot (there is one).

#include "common/row_recorder.hpp"

#include <unitree/idl/hg/LowState_.hpp>

#include <memory>
#include <string>

namespace unitree::robot {
template <typename T> class ChannelSubscriber;
}

namespace kist {

// G1 low-level state topic (unitree_hg::msg::dds_::LowState_).
inline constexpr const char* kLowStateTopic = "rt/lowstate";

class LowstateRecorder {
public:
    LowstateRecorder();
    ~LowstateRecorder();

    // Opens <session_dir>/lowstate.csv and starts the subscriber + writer.
    bool start(int domain_id, const std::string& network_interface,
               const std::string& session_dir, size_t queue_capacity,
               const std::string& topic = kLowStateTopic);

    // Stops the subscriber first (no more records), then drains and closes
    // the writer — every record received before stop() is on disk after.
    void stop();

    StreamStats stats() const { return rec_.stats(); }

    // internal: DDS callback
    void on_lowstate(const void* message);

private:
    using Sub = unitree::robot::ChannelSubscriber<unitree_hg::msg::dds_::LowState_>;
    std::unique_ptr<Sub> sub_;

    RowRecorder<unitree_hg::msg::dds_::LowState_> rec_;
    bool running_ = false;
};

} // namespace kist
