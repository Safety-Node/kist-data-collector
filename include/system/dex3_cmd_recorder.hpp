#pragma once

// Dex3 hand-command recording assembly — the action twin of Dex3Recorder,
// one instance per hand:
//
//   [DDS rt/dex3/<side>/cmd] --callback--> [RowRecorder] -> hand_cmd_<side>.csv
//
// A command topic is silent while no hand controller publishes: time gaps
// = no commands, not loss. Each instance owns its subscriber and its
// RowRecorder (own queue + writer thread), independent of the state
// recorders (columns: unitree_recorder/dex3_cmd_rows.hpp).

#include "common/row_recorder.hpp"

#include <unitree/idl/hg/HandCmd_.hpp>

#include <memory>
#include <string>

namespace unitree::robot {
template <typename T> class ChannelSubscriber;
}

namespace kist {

// Dex3 hand command topic: rt/dex3/<side>/cmd, side = "left" | "right".
inline std::string dex3_cmd_topic(const std::string& side) {
    return "rt/dex3/" + side + "/cmd";
}

class Dex3CmdRecorder {
public:
    Dex3CmdRecorder();
    ~Dex3CmdRecorder();

    // Opens <session_dir>/hand_cmd_<side>.csv and starts the subscriber + writer.
    bool start(int domain_id, const std::string& network_interface,
               const std::string& session_dir, const std::string& side,
               size_t queue_capacity);

    void stop();

    const std::string& side() const { return side_; }
    StreamStats stats() const { return rec_.stats(); }

    // internal: DDS callback
    void on_hand_cmd(const void* message);

private:
    using Sub = unitree::robot::ChannelSubscriber<unitree_hg::msg::dds_::HandCmd_>;
    std::unique_ptr<Sub> sub_;

    RowRecorder<unitree_hg::msg::dds_::HandCmd_> rec_;
    std::string side_;
    bool        running_      = false;
    bool        sizes_logged_ = false;   // one-time size report (DDS thread only)
};

} // namespace kist
