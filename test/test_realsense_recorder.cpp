// Records ONLY the cameras in `realsense_cameras` — drives one
// system/RealsenseRecorder per camera by itself (the kist_data_collector
// runner records everything enabled). Also the reference for embedding it.
//
//   ./test_realsense_recorder [config_path]      (default config/config.yaml)

#include "common/config.hpp"
#include "common/dds_config.hpp"
#include "common/human_size.hpp"
#include "common/session.hpp"
#include "system/realsense_recorder.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
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

    const auto rs = root["realsense_cameras"];
    const size_t capacity = rs ? rs["queue_capacity"].as<size_t>(1024) : 1024;
    const bool reliable   = rs ? rs["reliable"].as<bool>(true) : true;
    std::vector<std::string> names;
    if (rs && rs["enabled"].as<bool>(true) && rs["cameras"] && rs["cameras"].IsSequence()) {
        for (const auto& cam : rs["cameras"]) {
            if (!cam["enabled"].as<bool>(true)) continue;
            if (const auto name = cam["name"].as<std::string>(""); !name.empty())
                names.push_back(name);
        }
    }
    if (names.empty()) {
        std::cerr << "[test_realsense_recorder] no camera enabled in " << config_path << "\n";
        return 1;
    }

    std::string dds_uri;
    if (!kist::apply_dds_config(root, &dds_uri)) return 1;

    const auto session = kist::session_create(output_dir);
    if (session.dir.empty()) return 1;

    std::vector<std::unique_ptr<kist::RealsenseRecorder>> recorders;
    std::vector<std::string> started;
    for (const auto& name : names) {
        auto rec = std::make_unique<kist::RealsenseRecorder>();
        if (!rec->start(domain_id, name, session.dir, capacity, reliable)) {
            std::cerr << "[test_realsense_recorder] camera '" << name << "' failed — skipped\n";
            continue;
        }
        started.push_back(name);
        recorders.push_back(std::move(rec));
    }
    if (recorders.empty()) return 1;
    kist::session_write_meta(session, domain_id, dds_uri, started);
    std::printf("[test_realsense_recorder] recording %zu camera(s) -> %s (domain=%d)\n",
                recorders.size(), session.dir.c_str(), domain_id);

    std::signal(SIGINT,  [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });

    struct Last { uint64_t c_rx = 0, c_wr = 0, d_rx = 0, d_wr = 0; };
    std::vector<Last> last(recorders.size());
    auto window = std::chrono::steady_clock::now();
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto now = std::chrono::steady_clock::now();
        if (now - window < std::chrono::seconds(1)) continue;
        window = now;
        for (size_t i = 0; i < recorders.size(); ++i) {
            const auto c = recorders[i]->color_stats();
            const auto d = recorders[i]->depth_stats();
            std::printf("  %-12s color rx %2llu wr %2llu fps drop %llu gap %llu %s | "
                        "depth rx %2llu wr %2llu fps drop %llu gap %llu %s\n",
                        recorders[i]->name().c_str(),
                        (unsigned long long)(c.received - last[i].c_rx),
                        (unsigned long long)(c.written  - last[i].c_wr),
                        (unsigned long long)c.dropped, (unsigned long long)c.wire_gaps,
                        kist::human_size(c.bytes).c_str(),
                        (unsigned long long)(d.received - last[i].d_rx),
                        (unsigned long long)(d.written  - last[i].d_wr),
                        (unsigned long long)d.dropped, (unsigned long long)d.wire_gaps,
                        kist::human_size(d.bytes).c_str());
            last[i] = {c.received, c.written, d.received, d.written};
        }
    }

    std::vector<kist::StreamSummary> summary;
    for (auto& rec : recorders) {
        rec->stop();
        summary.push_back({rec->name(), "color", rec->color_stats()});
        summary.push_back({rec->name(), "depth", rec->depth_stats()});
    }
    kist::session_finalize_meta(session, summary);
    std::printf("[test_realsense_recorder] session %s closed\n", session.dir.c_str());
    return 0;
}
