// kist_data_collector — records every enabled stream (cameras, lowstate,
// dex3 hands, UWB) into one session directory (compressed payloads verbatim
// + CSV indices; see common/session.hpp for the layout). Prints per-second
// per-stream rx/write fps and the loss counters; Ctrl-C / SIGTERM stops,
// drains the queues, and appends the summary to meta.yaml.
//
//   ./kist_data_collector [config_path]      (default config/config.yaml)

#include "common/config.hpp"
#include "common/dds_config.hpp"
#include "system/data_collector.hpp"

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

    auto settings = kist::DataCollector::Settings::from_yaml(root);
    if (!kist::apply_dds_config(root, &settings.dds_uri)) return 1;

    kist::DataCollector collector;
    if (!collector.start(settings)) return 1;

    std::signal(SIGINT,  [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });

    auto window = std::chrono::steady_clock::now();
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto now = std::chrono::steady_clock::now();
        if (now - window < std::chrono::seconds(1)) continue;
        window = now;
        collector.print_report();
    }
    collector.stop();
    return 0;
}
