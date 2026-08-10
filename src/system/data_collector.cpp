#include "system/data_collector.hpp"

#include "common/human_size.hpp"

#include <cstdio>
#include <iostream>

namespace kist {

DataCollector::Settings DataCollector::Settings::from_yaml(const YAML::Node& root) {
    Settings s;
    // Absent-section guards: chaining [] on a missing yaml node throws.
    const auto unitree = root["unitree"];
    s.domain_id  = unitree ? unitree["domain_id"].as<int>(0) : 0;
    const auto storage = root["storage"];
    s.output_dir = storage ? storage["output_dir"].as<std::string>("sessions") : "sessions";

    // realsense_cameras: section-level enabled/queue_capacity, then the
    // camera list — each entry may carry its own enabled (default true).
    // A config without the section (or without cameras:) degrades to "none".
    const auto rs = root["realsense_cameras"];
    if (rs) {
        s.camera_queue_capacity = rs["queue_capacity"].as<size_t>(s.camera_queue_capacity);
        s.camera_reliable       = rs["reliable"].as<bool>(s.camera_reliable);
        if (rs["enabled"].as<bool>(true) && rs["cameras"] && rs["cameras"].IsSequence()) {
            for (const auto& cam : rs["cameras"]) {
                if (!cam["enabled"].as<bool>(true)) continue;
                if (const auto name = cam["name"].as<std::string>(""); !name.empty())
                    s.cameras.push_back(name);
            }
        }
    }

    if (const auto ls = root["lowstate"]; ls) {
        s.lowstate_enabled        = ls["enabled"].as<bool>(false);
        s.lowstate_queue_capacity = ls["queue_capacity"].as<size_t>(s.lowstate_queue_capacity);
    }
    if (const auto dx = root["dex3"]; dx) {
        s.dex3_enabled        = dx["enabled"].as<bool>(false);
        s.dex3_queue_capacity = dx["queue_capacity"].as<size_t>(s.dex3_queue_capacity);
    }
    if (const auto uw = root["uwb"]; uw) {
        s.uwb_enabled        = uw["enabled"].as<bool>(false);
        s.uwb_queue_capacity = uw["queue_capacity"].as<size_t>(s.uwb_queue_capacity);
    }
    if (const auto lc = root["lowcmd"]; lc) {
        s.lowcmd_enabled        = lc["enabled"].as<bool>(false);
        s.lowcmd_queue_capacity = lc["queue_capacity"].as<size_t>(s.lowcmd_queue_capacity);
    }
    if (const auto as = root["arm_sdk"]; as) {
        s.arm_sdk_enabled        = as["enabled"].as<bool>(false);
        s.arm_sdk_queue_capacity = as["queue_capacity"].as<size_t>(s.arm_sdk_queue_capacity);
    }
    if (const auto dc = root["dex3_cmd"]; dc) {
        s.dex3_cmd_enabled        = dc["enabled"].as<bool>(false);
        s.dex3_cmd_queue_capacity = dc["queue_capacity"].as<size_t>(s.dex3_cmd_queue_capacity);
    }
    s.task = root["task"] ? root["task"].as<std::string>("") : "";
    return s;
}

bool DataCollector::start(const Settings& settings) {
    if (running_) return true;
    settings_ = settings;

    if (settings_.cameras.empty() && !settings_.lowstate_enabled &&
        !settings_.dex3_enabled && !settings_.uwb_enabled &&
        !settings_.lowcmd_enabled && !settings_.arm_sdk_enabled &&
        !settings_.dex3_cmd_enabled) {
        std::cerr << "[kist_data_collector] nothing to record — no stream enabled in config\n";
        return false;
    }
    if (settings_.task.empty())
        std::cerr << "[kist_data_collector] warning: no `task:` in config — "
                     "meta.yaml gets an empty task label\n";

    session_ = session_create(settings_.output_dir);
    if (session_.dir.empty()) return false;

    // Empty on purpose — the NIC comes from the DDS config XML (see
    // common/dds_config.hpp).
    const std::string sdk_iface;

    std::vector<std::string> started;
    for (const auto& name : settings_.cameras) {
        auto rec = std::make_unique<RealsenseRecorder>();
        if (!rec->start(settings_.domain_id, name, session_.dir,
                        settings_.camera_queue_capacity, settings_.camera_reliable)) {
            std::cerr << "[kist_data_collector] camera '" << name << "' failed — skipped\n";
            continue;
        }
        started.push_back(name);
        cameras_.push_back(std::move(rec));
    }

    if (settings_.lowstate_enabled) {
        lowstate_ = std::make_unique<LowstateRecorder>();
        if (!lowstate_->start(settings_.domain_id, sdk_iface, session_.dir,
                              settings_.lowstate_queue_capacity)) {
            std::cerr << "[kist_data_collector] lowstate failed — skipped\n";
            lowstate_.reset();
        }
    }

    if (settings_.dex3_enabled) {
        for (const std::string side : {"left", "right"}) {
            auto hand = std::make_unique<Dex3Recorder>();
            if (!hand->start(settings_.domain_id, sdk_iface, session_.dir, side,
                             settings_.dex3_queue_capacity)) {
                std::cerr << "[kist_data_collector] dex3 " << side << " failed — skipped\n";
                continue;
            }
            hands_.push_back(std::move(hand));
        }
    }

    if (settings_.uwb_enabled) {
        uwb_ = std::make_unique<UwbRecorder>();
        if (!uwb_->start(settings_.domain_id, sdk_iface, session_.dir,
                         settings_.uwb_queue_capacity)) {
            std::cerr << "[kist_data_collector] uwb failed — skipped\n";
            uwb_.reset();
        }
    }

    // Command (action) streams: rt/lowcmd and rt/arm_sdk share the
    // LowCmd_ recorder (topic + csv name differ); hand commands get one
    // recorder per side, like the state twins.
    struct BodyCmd { bool enabled; size_t capacity; const char* topic; const char* csv; };
    for (const auto& bc : {
             BodyCmd{settings_.lowcmd_enabled, settings_.lowcmd_queue_capacity,
                     kLowCmdTopic, "lowcmd.csv"},
             BodyCmd{settings_.arm_sdk_enabled, settings_.arm_sdk_queue_capacity,
                     kArmSdkTopic, "arm_sdk.csv"}}) {
        if (!bc.enabled) continue;
        auto rec = std::make_unique<LowcmdRecorder>();
        if (!rec->start(settings_.domain_id, sdk_iface, session_.dir,
                        bc.capacity, bc.topic, bc.csv)) {
            std::cerr << "[kist_data_collector] " << bc.topic << " failed — skipped\n";
            continue;
        }
        body_cmds_.push_back(std::move(rec));
    }

    if (settings_.dex3_cmd_enabled) {
        for (const std::string side : {"left", "right"}) {
            auto rec = std::make_unique<Dex3CmdRecorder>();
            if (!rec->start(settings_.domain_id, sdk_iface, session_.dir, side,
                            settings_.dex3_cmd_queue_capacity)) {
                std::cerr << "[kist_data_collector] dex3 cmd " << side << " failed — skipped\n";
                continue;
            }
            hand_cmds_.push_back(std::move(rec));
        }
    }

    if (cameras_.empty() && !lowstate_ && hands_.empty() && !uwb_ &&
        body_cmds_.empty() && hand_cmds_.empty()) {
        std::cerr << "[kist_data_collector] no streams started\n";
        return false;
    }
    session_write_meta(session_, settings_.domain_id, settings_.dds_uri, started,
                       settings_.task);

    last_cam_.assign(cameras_.size(), {});
    last_hand_.assign(hands_.size(), {0, 0});
    last_body_cmd_.assign(body_cmds_.size(), {0, 0});
    last_hand_cmd_.assign(hand_cmds_.size(), {0, 0});
    std::printf("[kist_data_collector] recording %zu camera(s)%s + %zu hand(s) + "
                "%zu cmd stream(s) -> %s (domain=%d)\n",
                cameras_.size(), lowstate_ ? " + lowstate" : "", hands_.size(),
                body_cmds_.size() + hand_cmds_.size(),
                session_.dir.c_str(), settings_.domain_id);
    if (!settings_.task.empty())
        std::printf("[kist_data_collector] task: %s\n", settings_.task.c_str());
    running_ = true;
    return true;
}

void DataCollector::print_report() {
    for (size_t i = 0; i < cameras_.size(); ++i) {
        const auto c = cameras_[i]->color_stats();
        const auto d = cameras_[i]->depth_stats();
        std::printf("  %-12s color rx %2llu wr %2llu fps drop %llu gap %llu %s | "
                    "depth rx %2llu wr %2llu fps drop %llu gap %llu %s\n",
                    cameras_[i]->name().c_str(),
                    (unsigned long long)(c.received - last_cam_[i].c_rx),
                    (unsigned long long)(c.written  - last_cam_[i].c_wr),
                    (unsigned long long)c.dropped, (unsigned long long)c.wire_gaps,
                    human_size(c.bytes).c_str(),
                    (unsigned long long)(d.received - last_cam_[i].d_rx),
                    (unsigned long long)(d.written  - last_cam_[i].d_wr),
                    (unsigned long long)d.dropped, (unsigned long long)d.wire_gaps,
                    human_size(d.bytes).c_str());
        last_cam_[i] = {c.received, c.written, d.received, d.written};
    }
    if (lowstate_) {
        const auto l = lowstate_->stats();
        std::printf("  %-12s rx %4llu wr %4llu hz drop %llu werr %llu %s\n",
                    "lowstate",
                    (unsigned long long)(l.received - last_ls_rx_),
                    (unsigned long long)(l.written  - last_ls_wr_),
                    (unsigned long long)l.dropped, (unsigned long long)l.write_errors,
                    human_size(l.bytes).c_str());
        last_ls_rx_ = l.received;
        last_ls_wr_ = l.written;
    }
    if (uwb_) {
        const auto u = uwb_->stats();
        std::printf("  %-12s rx %4llu wr %4llu hz drop %llu werr %llu %s\n",
                    "uwb",
                    (unsigned long long)(u.received - last_uw_rx_),
                    (unsigned long long)(u.written  - last_uw_wr_),
                    (unsigned long long)u.dropped, (unsigned long long)u.write_errors,
                    human_size(u.bytes).c_str());
        last_uw_rx_ = u.received;
        last_uw_wr_ = u.written;
    }
    for (size_t i = 0; i < hands_.size(); ++i) {
        const auto h = hands_[i]->stats();
        std::printf("  hand_%-7s rx %4llu wr %4llu hz drop %llu werr %llu %s\n",
                    hands_[i]->side().c_str(),
                    (unsigned long long)(h.received - last_hand_[i].first),
                    (unsigned long long)(h.written  - last_hand_[i].second),
                    (unsigned long long)h.dropped, (unsigned long long)h.write_errors,
                    human_size(h.bytes).c_str());
        last_hand_[i] = {h.received, h.written};
    }
    for (size_t i = 0; i < body_cmds_.size(); ++i) {
        const auto c = body_cmds_[i]->stats();
        std::printf("  %-12s rx %4llu wr %4llu hz drop %llu werr %llu %s\n",
                    body_cmds_[i]->label().c_str(),
                    (unsigned long long)(c.received - last_body_cmd_[i].first),
                    (unsigned long long)(c.written  - last_body_cmd_[i].second),
                    (unsigned long long)c.dropped, (unsigned long long)c.write_errors,
                    human_size(c.bytes).c_str());
        last_body_cmd_[i] = {c.received, c.written};
    }
    for (size_t i = 0; i < hand_cmds_.size(); ++i) {
        const auto c = hand_cmds_[i]->stats();
        std::printf("  cmd_%-8s rx %4llu wr %4llu hz drop %llu werr %llu %s\n",
                    hand_cmds_[i]->side().c_str(),
                    (unsigned long long)(c.received - last_hand_cmd_[i].first),
                    (unsigned long long)(c.written  - last_hand_cmd_[i].second),
                    (unsigned long long)c.dropped, (unsigned long long)c.write_errors,
                    human_size(c.bytes).c_str());
        last_hand_cmd_[i] = {c.received, c.written};
    }
}

void DataCollector::stop() {
    if (!running_) return;
    std::printf("[kist_data_collector] stopping — draining queues\n");
    std::vector<StreamSummary> summary;
    for (auto& rec : cameras_) {
        rec->stop();
        summary.push_back({rec->name(), "color", rec->color_stats()});
        summary.push_back({rec->name(), "depth", rec->depth_stats()});
    }
    if (lowstate_) {
        lowstate_->stop();
        summary.push_back({"unitree", "lowstate", lowstate_->stats()});
    }
    for (auto& hand : hands_) {
        hand->stop();
        summary.push_back({"dex3", "hand_" + hand->side(), hand->stats()});
    }
    if (uwb_) {
        uwb_->stop();
        summary.push_back({"uwb", "position", uwb_->stats()});
    }
    for (auto& rec : body_cmds_) {
        rec->stop();
        summary.push_back({"unitree", rec->label(), rec->stats()});
    }
    for (auto& rec : hand_cmds_) {
        rec->stop();
        summary.push_back({"dex3", "hand_cmd_" + rec->side(), rec->stats()});
    }
    session_finalize_meta(session_, summary);

    for (const auto& s : summary)
        std::printf("  %-12s %-5s received %llu written %llu dropped %llu "
                    "write_errors %llu wire_gaps %llu\n",
                    s.source.c_str(), s.stream.c_str(),
                    (unsigned long long)s.stats.received, (unsigned long long)s.stats.written,
                    (unsigned long long)s.stats.dropped, (unsigned long long)s.stats.write_errors,
                    (unsigned long long)s.stats.wire_gaps);
    std::printf("[kist_data_collector] session %s closed\n", session_.dir.c_str());

    cameras_.clear();
    lowstate_.reset();
    hands_.clear();
    uwb_.reset();
    body_cmds_.clear();
    hand_cmds_.clear();
    running_ = false;
}

} // namespace kist
