#pragma once

#include "common/session.hpp"
#include "system/realsense_recorder.hpp"
#include "system/dex3_cmd_recorder.hpp"
#include "system/dex3_recorder.hpp"
#include "system/lowcmd_recorder.hpp"
#include "system/lowstate_recorder.hpp"
#include "system/motion_token_recorder.hpp"
#include "system/uwb_recorder.hpp"

#include <yaml-cpp/yaml.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace kist {

// Whole-app assembly — every enabled recorder behind one start/stop, into
// one session directory. This is what the kist_data_collector runner
// drives; the per-stream recorders stay directly embeddable.
class DataCollector {
public:
    struct Settings {
        int         domain_id = 0;
        std::string dds_uri;                // recorded into meta.yaml
        std::string output_dir = "sessions";
        // Language instruction for this recording (config `task:`) —
        // recorded into meta.yaml; the LeRobot/GR00T export reads it as
        // the episode's task description.
        std::string task;

        std::vector<std::string> cameras;   // enabled camera names
        size_t camera_queue_capacity = 1024;
        // RELIABLE readers recover isolated wire losses via RTPS
        // retransmission (the SDK writers offer it); false = plain
        // best-effort, the safety valve if a transmitter ever stops
        // offering RELIABLE.
        bool   camera_reliable = true;

        bool   lowstate_enabled = false;
        // ~1 kHz — the camera default would give only ~1 s of disk-stall
        // headroom (rows are small, so 8192 is ~25 MB).
        size_t lowstate_queue_capacity = 8192;

        bool   dex3_enabled = false;
        size_t dex3_queue_capacity = 4096;

        bool   uwb_enabled = false;
        size_t uwb_queue_capacity = 256;

        // Command (action) streams — silent while no controller publishes.
        bool   lowcmd_enabled = false;      // rt/lowcmd, up to ~1 kHz
        size_t lowcmd_queue_capacity = 8192;
        bool   arm_sdk_enabled = false;     // rt/arm_sdk (same LowCmd_ type)
        size_t arm_sdk_queue_capacity = 8192;
        bool   dex3_cmd_enabled = false;    // rt/dex3/{left,right}/cmd
        size_t dex3_cmd_queue_capacity = 4096;
        // Gearsonic decoder-input tokens (50 Hz) — silent while the robot
        // is outside CONTROL.
        bool   motion_token_enabled = false;  // rt/kist/motion_token
        size_t motion_token_queue_capacity = 4096;

        // config.yaml -> Settings (dds_uri is filled by apply_dds_config).
        static Settings from_yaml(const YAML::Node& root);
    };

    // Creates the session dir, starts every enabled recorder (a failing
    // stream is skipped, not fatal), writes meta.yaml, prints the banner.
    // False when nothing is enabled or nothing started.
    bool start(const Settings& settings);

    // One report block: per-stream rx/write fps and the loss counters that
    // must stay 0 (drop = our queue, gap = lost on the wire). Call at 1 Hz.
    void print_report();

    // Drains the queues, appends the summary to meta.yaml, prints it.
    void stop();

    // Appends the operator's success/fail label to meta.yaml. Call after
    // stop(); empty result = leave the episode unlabeled.
    void write_result(const std::string& result);

    const std::string& session_dir() const { return session_.dir; }

private:
    Settings    settings_;
    SessionInfo session_;

    std::vector<std::unique_ptr<RealsenseRecorder>> cameras_;
    std::unique_ptr<LowstateRecorder>               lowstate_;
    std::vector<std::unique_ptr<Dex3Recorder>>      hands_;
    std::unique_ptr<UwbRecorder>                    uwb_;
    std::vector<std::unique_ptr<LowcmdRecorder>>    body_cmds_;   // lowcmd / arm_sdk
    std::vector<std::unique_ptr<Dex3CmdRecorder>>   hand_cmds_;
    std::unique_ptr<MotionTokenRecorder>            motion_token_;

    // print_report deltas (previous window's counters).
    struct CamLast { uint64_t c_rx = 0, c_wr = 0, d_rx = 0, d_wr = 0; };
    std::vector<CamLast>                       last_cam_;
    uint64_t                                   last_ls_rx_ = 0, last_ls_wr_ = 0;
    uint64_t                                   last_uw_rx_ = 0, last_uw_wr_ = 0;
    std::vector<std::pair<uint64_t, uint64_t>> last_hand_;
    std::vector<std::pair<uint64_t, uint64_t>> last_body_cmd_;
    std::vector<std::pair<uint64_t, uint64_t>> last_hand_cmd_;
    uint64_t                                   last_mt_rx_ = 0, last_mt_wr_ = 0;

    bool running_ = false;
};

} // namespace kist
