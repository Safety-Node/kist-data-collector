#pragma once

// Dex3 hand-state recording assembly — one instance per hand, the finger
// sibling of LowstateRecorder (fingers are NOT in rt/lowstate; each hand
// publishes its own topic):
//
//   [DDS rt/dex3/<side>/state] --callback--> [RowRecorder] -> hand_<side>.csv
//
// Each instance owns its subscriber and its RowRecorder, i.e. its own queue
// and writer thread — fully independent of the lowstate recorder and of the
// other hand. Columns: recv_ns, 7 finger motors × q/dq/ddq/tau_est, and the
// fingertip press-sensor pads (unitree_recorder/dex3_rows.hpp has the layout).

#include "common/row_recorder.hpp"

#include <unitree/idl/hg/HandState_.hpp>

#include <memory>
#include <string>

namespace unitree::robot {
template <typename T> class ChannelSubscriber;
}

namespace kist {

// Dex3 hand state topic: rt/dex3/<side>/state, side = "left" | "right".
inline std::string dex3_state_topic(const std::string& side) {
    return "rt/dex3/" + side + "/state";
}

class Dex3Recorder {
public:
    Dex3Recorder();
    ~Dex3Recorder();

    // Opens <session_dir>/hand_<side>.csv and starts the subscriber + writer.
    bool start(int domain_id, const std::string& network_interface,
               const std::string& session_dir, const std::string& side,
               size_t queue_capacity);

    void stop();

    const std::string& side() const { return side_; }
    StreamStats stats() const { return rec_.stats(); }

    // internal: DDS callback
    void on_hand_state(const void* message);

private:
    using Sub = unitree::robot::ChannelSubscriber<unitree_hg::msg::dds_::HandState_>;
    std::unique_ptr<Sub> sub_;

    RowRecorder<unitree_hg::msg::dds_::HandState_> rec_;
    std::string side_;
    bool        running_      = false;
    bool        sizes_logged_ = false;   // one-time size report (DDS thread only)
};

} // namespace kist
