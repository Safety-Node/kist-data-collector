// Records ONLY the command (action) streams — drives system/LowcmdRecorder
// (rt/lowcmd + rt/arm_sdk) and system/Dex3CmdRecorder (both hands) by
// themselves, regardless of their `enabled` flags (the kist_data_collector
// runner records everything enabled). Command topics are silent while no
// controller publishes — 0 hz here just means nothing is commanding.
//
//   ./test_cmd_recorder [config_path]      (default config/config.yaml)

#include "common/config.hpp"
#include "common/dds_config.hpp"
#include "common/human_size.hpp"
#include "common/session.hpp"
#include "system/dex3_cmd_recorder.hpp"
#include "system/lowcmd_recorder.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

static std::atomic<bool> g_stop{false};

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const std::string config_path = (argc >= 2) ? argv[1] : "config/config.yaml";

    kist::Config::instance().load(config_path);
    const auto& root = kist::Config::instance().root();
    const auto unitree  = root["unitree"];
    const int domain_id = unitree ? unitree["domain_id"].as<int>(0) : 0;
    const auto storage  = root["storage"];
    const std::string output_dir =
        storage ? storage["output_dir"].as<std::string>("sessions") : "sessions";
    const auto lc = root["lowcmd"];
    const auto dc = root["dex3_cmd"];
    const size_t body_capacity = lc ? lc["queue_capacity"].as<size_t>(8192) : 8192;
    const size_t hand_capacity = dc ? dc["queue_capacity"].as<size_t>(4096) : 4096;

    std::string dds_uri;
    if (!kist::apply_dds_config(root, &dds_uri)) return 1;

    const auto session = kist::session_create(output_dir);
    if (session.dir.empty()) return 1;

    std::vector<std::unique_ptr<kist::LowcmdRecorder>> body;
    for (const auto& [topic, csv] :
         {std::pair{kist::kLowCmdTopic, "lowcmd.csv"},
          std::pair{kist::kArmSdkTopic, "arm_sdk.csv"}}) {
        auto rec = std::make_unique<kist::LowcmdRecorder>();
        if (!rec->start(domain_id, "", session.dir, body_capacity, topic, csv)) {
            std::cerr << "[test_cmd_recorder] " << topic << " failed — skipped\n";
            continue;
        }
        body.push_back(std::move(rec));
    }
    std::vector<std::unique_ptr<kist::Dex3CmdRecorder>> hands;
    for (const std::string side : {"left", "right"}) {
        auto rec = std::make_unique<kist::Dex3CmdRecorder>();
        if (!rec->start(domain_id, "", session.dir, side, hand_capacity)) {
            std::cerr << "[test_cmd_recorder] dex3 cmd " << side << " failed — skipped\n";
            continue;
        }
        hands.push_back(std::move(rec));
    }
    if (body.empty() && hands.empty()) return 1;
    kist::session_write_meta(session, domain_id, dds_uri, {});
    std::printf("[test_cmd_recorder] recording %zu cmd stream(s) -> %s (domain=%d)\n",
                body.size() + hands.size(), session.dir.c_str(), domain_id);

    std::signal(SIGINT,  [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });

    std::vector<std::pair<uint64_t, uint64_t>> last_b(body.size(), {0, 0});
    std::vector<std::pair<uint64_t, uint64_t>> last_h(hands.size(), {0, 0});
    auto window = std::chrono::steady_clock::now();
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto now = std::chrono::steady_clock::now();
        if (now - window < std::chrono::seconds(1)) continue;
        window = now;
        for (size_t i = 0; i < body.size(); ++i) {
            const auto s = body[i]->stats();
            std::printf("  %-12s rx %4llu wr %4llu hz drop %llu werr %llu %s\n",
                        body[i]->label().c_str(),
                        (unsigned long long)(s.received - last_b[i].first),
                        (unsigned long long)(s.written  - last_b[i].second),
                        (unsigned long long)s.dropped, (unsigned long long)s.write_errors,
                        kist::human_size(s.bytes).c_str());
            last_b[i] = {s.received, s.written};
        }
        for (size_t i = 0; i < hands.size(); ++i) {
            const auto s = hands[i]->stats();
            std::printf("  cmd_%-8s rx %4llu wr %4llu hz drop %llu werr %llu %s\n",
                        hands[i]->side().c_str(),
                        (unsigned long long)(s.received - last_h[i].first),
                        (unsigned long long)(s.written  - last_h[i].second),
                        (unsigned long long)s.dropped, (unsigned long long)s.write_errors,
                        kist::human_size(s.bytes).c_str());
            last_h[i] = {s.received, s.written};
        }
    }

    std::vector<kist::StreamSummary> summary;
    for (auto& rec : body) {
        rec->stop();
        summary.push_back({"unitree", rec->label(), rec->stats()});
    }
    for (auto& rec : hands) {
        rec->stop();
        summary.push_back({"dex3", "hand_cmd_" + rec->side(), rec->stats()});
    }
    kist::session_finalize_meta(session, summary);
    std::printf("[test_cmd_recorder] session %s closed\n", session.dir.c_str());
    return 0;
}
