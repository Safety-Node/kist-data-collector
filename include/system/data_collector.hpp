#pragma once

#include "common/session.hpp"
#include "realsense_recorder/realsense_recorder.hpp"
#include "unitree_recorder/dex3_recorder.hpp"
#include "unitree_recorder/lowstate_recorder.hpp"
#include "uwb_recorder/uwb_recorder.hpp"

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

    const std::string& session_dir() const { return session_.dir; }

private:
    Settings    settings_;
    SessionInfo session_;

    std::vector<std::unique_ptr<RealsenseRecorder>> cameras_;
    std::unique_ptr<LowstateRecorder>               lowstate_;
    std::vector<std::unique_ptr<Dex3Recorder>>      hands_;
    std::unique_ptr<UwbRecorder>                    uwb_;

    // print_report deltas (previous window's counters).
    struct CamLast { uint64_t c_rx = 0, c_wr = 0, d_rx = 0, d_wr = 0; };
    std::vector<CamLast>                       last_cam_;
    uint64_t                                   last_ls_rx_ = 0, last_ls_wr_ = 0;
    uint64_t                                   last_uw_rx_ = 0, last_uw_wr_ = 0;
    std::vector<std::pair<uint64_t, uint64_t>> last_hand_;

    bool running_ = false;
};

} // namespace kist
