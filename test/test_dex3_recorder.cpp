// Records ONLY the Dex3 hands (rt/dex3/<side>/state, ~830 Hz each) — drives
// one system/Dex3Recorder per side by itself (the kist_data_collector
// runner records everything enabled). Also the reference for embedding it.
//
//   ./test_dex3_recorder [config_path]      (default config/config.yaml)

#include "common/config.hpp"
#include "common/dds_config.hpp"
#include "common/human_size.hpp"
#include "common/session.hpp"
#include "system/dex3_recorder.hpp"

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
    const auto dx = root["dex3"];
    const size_t capacity = dx ? dx["queue_capacity"].as<size_t>(4096) : 4096;

    std::string dds_uri;
    if (!kist::apply_dds_config(root, &dds_uri)) return 1;

    const auto session = kist::session_create(output_dir);
    if (session.dir.empty()) return 1;

    std::vector<std::unique_ptr<kist::Dex3Recorder>> hands;
    for (const std::string side : {"left", "right"}) {
        auto hand = std::make_unique<kist::Dex3Recorder>();
        if (!hand->start(domain_id, "", session.dir, side, capacity)) {
            std::cerr << "[test_dex3_recorder] " << side << " failed — skipped\n";
            continue;
        }
        hands.push_back(std::move(hand));
    }
    if (hands.empty()) return 1;
    kist::session_write_meta(session, domain_id, dds_uri, {});
    std::printf("[test_dex3_recorder] recording %zu hand(s) -> %s (domain=%d)\n",
                hands.size(), session.dir.c_str(), domain_id);

    std::signal(SIGINT,  [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });

    std::vector<std::pair<uint64_t, uint64_t>> last(hands.size(), {0, 0});
    auto window = std::chrono::steady_clock::now();
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto now = std::chrono::steady_clock::now();
        if (now - window < std::chrono::seconds(1)) continue;
        window = now;
        for (size_t i = 0; i < hands.size(); ++i) {
            const auto s = hands[i]->stats();
            std::printf("  hand_%-7s rx %4llu wr %4llu hz drop %llu werr %llu %s\n",
                        hands[i]->side().c_str(),
                        (unsigned long long)(s.received - last[i].first),
                        (unsigned long long)(s.written  - last[i].second),
                        (unsigned long long)s.dropped, (unsigned long long)s.write_errors,
                        kist::human_size(s.bytes).c_str());
            last[i] = {s.received, s.written};
        }
    }

    std::vector<kist::StreamSummary> summary;
    for (auto& hand : hands) {
        hand->stop();
        summary.push_back({"dex3", "hand_" + hand->side(), hand->stats()});
    }
    kist::session_finalize_meta(session, summary);
    std::printf("[test_dex3_recorder] session %s closed\n", session.dir.c_str());
    return 0;
}
