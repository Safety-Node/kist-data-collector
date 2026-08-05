// Records ONLY the UWB fixes (rt/kist/uwb/pose, ~10 Hz) — drives the
// system/UwbRecorder assembly by itself (the kist_data_collector runner
// records everything enabled). Also the reference for embedding it.
//
//   ./test_uwb_recorder [config_path]      (default config/config.yaml)

#include "common/config.hpp"
#include "common/dds_config.hpp"
#include "common/human_size.hpp"
#include "common/session.hpp"
#include "system/uwb_recorder.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>

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
    const auto uw = root["uwb"];
    const size_t capacity = uw ? uw["queue_capacity"].as<size_t>(256) : 256;

    std::string dds_uri;
    if (!kist::apply_dds_config(root, &dds_uri)) return 1;

    const auto session = kist::session_create(output_dir);
    if (session.dir.empty()) return 1;

    kist::UwbRecorder rec;
    if (!rec.start(domain_id, "", session.dir, capacity)) return 1;
    kist::session_write_meta(session, domain_id, dds_uri, {});
    std::printf("[test_uwb_recorder] recording -> %s (domain=%d)\n",
                session.dir.c_str(), domain_id);

    std::signal(SIGINT,  [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });

    uint64_t last_rx = 0, last_wr = 0;
    auto window = std::chrono::steady_clock::now();
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto now = std::chrono::steady_clock::now();
        if (now - window < std::chrono::seconds(1)) continue;
        window = now;
        const auto s = rec.stats();
        std::printf("  uwb          rx %4llu wr %4llu hz drop %llu werr %llu %s\n",
                    (unsigned long long)(s.received - last_rx),
                    (unsigned long long)(s.written  - last_wr),
                    (unsigned long long)s.dropped, (unsigned long long)s.write_errors,
                    kist::human_size(s.bytes).c_str());
        last_rx = s.received;
        last_wr = s.written;
    }

    rec.stop();
    kist::session_finalize_meta(session, {{"uwb", "position", rec.stats()}});
    std::printf("[test_uwb_recorder] session %s closed\n", session.dir.c_str());
    return 0;
}
