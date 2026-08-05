// kist_data_collector — records every camera in `realsense_cameras` into one
// session directory (compressed payloads verbatim + CSV indices; see
// common/session.hpp for the layout). Prints per-second per-stream
// rx/write fps and the loss counters; Ctrl-C / SIGTERM stops, drains the
// queues, and appends the summary to meta.yaml.
//
//   ./kist_data_collector [config_path]      (default config/config.yaml)

#include "realsense_recorder/realsense_recorder.hpp"
#include "unitree_recorder/lowstate_recorder.hpp"
#include "unitree_recorder/dex3_recorder.hpp"
#include "uwb_recorder/uwb_recorder.hpp"
#include "common/session.hpp"
#include "common/config.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

static std::atomic<bool> g_stop{false};

// Human-readable size for the 1 Hz report: B / KB / MB. Thin streams (UWB,
// ~650 B/s) would otherwise sit at "0.0 MB" for minutes.
static std::string human_size(uint64_t bytes) {
    char b[32];
    if (bytes >= 1000000)   std::snprintf(b, sizeof(b), "%6.1f MB", double(bytes) / 1e6);
    else if (bytes >= 1000) std::snprintf(b, sizeof(b), "%6.1f KB", double(bytes) / 1e3);
    else                    std::snprintf(b, sizeof(b), "%4llu B ", (unsigned long long)bytes);
    return b;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const std::string config_path = (argc >= 2) ? argv[1] : "config/config.yaml";

    kist::Config::instance().load(config_path);
    const auto& root = kist::Config::instance().root();

    const auto unitree  = root["unitree"];
    const int domain_id = unitree["domain_id"].as<int>(0);

    // DDS transport config (network interface + receive tuning) lives in an
    // XML file — config/cyclonedds.xml by default. It MUST be routed via
    // CYCLONEDDS_URI with the SDK given an EMPTY interface: unitree's
    // ChannelFactory::Init(domain, iface) with a non-empty iface builds its
    // own DDS config and silently discards CYCLONEDDS_URI (measured —
    // sockets stayed at Cyclone defaults). Silent misconfig burned us once,
    // so this is validated loudly: missing file = fatal, and the effective
    // URI is printed at startup. A pre-set CYCLONEDDS_URI wins (power users).
    const std::string dds_config =
        unitree["dds_config"].as<std::string>("config/cyclonedds.xml");
    std::string dds_uri;
    if (const char* prev = std::getenv("CYCLONEDDS_URI"); prev && *prev) {
        dds_uri = prev;
    } else {
        std::error_code ec;
        const auto abs = std::filesystem::absolute(dds_config, ec);
        if (ec || !std::filesystem::exists(abs)) {
            std::cerr << "[kist_data_collector] DDS config not found: " << dds_config
                      << " — set unitree.dds_config (network interface and "
                         "buffer tuning live there, NOT in config.yaml)\n";
            return 1;
        }
        dds_uri = "file://" + abs.string();
        setenv("CYCLONEDDS_URI", dds_uri.c_str(), 1);
    }
    std::printf("[kist_data_collector] DDS config: %s\n", dds_uri.c_str());
    const std::string sdk_iface;  // empty on purpose — see above

    const std::string output_dir = root["storage"]["output_dir"].as<std::string>("sessions");

    // Absent-section guards: chaining [] on a missing yaml node throws.
    const auto ls_node = root["lowstate"];
    const auto dx_node = root["dex3"];
    const auto uw_node = root["uwb"];
    const bool lowstate_enabled = ls_node && ls_node["enabled"].as<bool>(false);
    const bool dex3_enabled     = dx_node && dx_node["enabled"].as<bool>(false);
    const bool uwb_enabled      = uw_node && uw_node["enabled"].as<bool>(false);

    // realsense_cameras: section-level enabled/queue_capacity, then the
    // camera list — each entry may carry its own enabled (default true).
    // Guards matter: iterating an absent yaml node aborts, so a config
    // without the section (or without cameras:) must degrade to "none".
    const auto rs = root["realsense_cameras"];
    const size_t rs_capacity = rs ? rs["queue_capacity"].as<size_t>(1024) : 1024;
    // RELIABLE readers recover isolated wire losses via RTPS retransmission
    // (the SDK writers offer it); false = plain best-effort, the safety
    // valve if a transmitter ever stops offering RELIABLE.
    const bool rs_reliable = rs ? rs["reliable"].as<bool>(true) : true;
    std::vector<std::string> names;
    if (rs && rs["enabled"].as<bool>(true) && rs["cameras"] && rs["cameras"].IsSequence()) {
        for (const auto& cam : rs["cameras"]) {
            if (!cam["enabled"].as<bool>(true)) continue;
            if (const auto name = cam["name"].as<std::string>(""); !name.empty())
                names.push_back(name);
        }
    }

    if (names.empty() && !lowstate_enabled && !dex3_enabled && !uwb_enabled) {
        std::cerr << "[kist_data_collector] nothing to record in " << config_path << "\n";
        return 1;
    }

    const auto session = kist::session_create(output_dir);
    if (session.dir.empty()) return 1;

    std::vector<std::unique_ptr<kist::RealsenseRecorder>> recorders;
    std::vector<std::string> started;
    for (const auto& name : names) {
        auto rec = std::make_unique<kist::RealsenseRecorder>();
        if (!rec->start(domain_id, name, session.dir, rs_capacity, rs_reliable)) {
            std::cerr << "[kist_data_collector] camera '" << name << "' failed — skipped\n";
            continue;
        }
        started.push_back(name);
        recorders.push_back(std::move(rec));
    }

    std::unique_ptr<kist::LowstateRecorder> lowstate;
    if (lowstate_enabled) {
        // Own queue bound: ~1 kHz means the camera default would give only
        // ~1 s of disk-stall headroom (rows are small, so 8192 is ~25 MB).
        const size_t ls_capacity = ls_node["queue_capacity"].as<size_t>(8192);
        lowstate = std::make_unique<kist::LowstateRecorder>();
        if (!lowstate->start(domain_id, sdk_iface, session.dir, ls_capacity)) {
            std::cerr << "[kist_data_collector] lowstate failed — skipped\n";
            lowstate.reset();
        }
    }

    std::vector<std::unique_ptr<kist::Dex3Recorder>> hands;
    if (dex3_enabled) {
        const size_t dex3_capacity = dx_node["queue_capacity"].as<size_t>(4096);
        for (const std::string side : {"left", "right"}) {
            auto hand = std::make_unique<kist::Dex3Recorder>();
            if (!hand->start(domain_id, sdk_iface, session.dir, side, dex3_capacity)) {
                std::cerr << "[kist_data_collector] dex3 " << side << " failed — skipped\n";
                continue;
            }
            hands.push_back(std::move(hand));
        }
    }

    std::unique_ptr<kist::UwbRecorder> uwb;
    if (uwb_enabled) {
        const size_t uwb_capacity = uw_node["queue_capacity"].as<size_t>(256);
        uwb = std::make_unique<kist::UwbRecorder>();
        if (!uwb->start(domain_id, sdk_iface, session.dir, uwb_capacity)) {
            std::cerr << "[kist_data_collector] uwb failed — skipped\n";
            uwb.reset();
        }
    }

    if (recorders.empty() && !lowstate && hands.empty() && !uwb) {
        std::cerr << "[kist_data_collector] no streams started\n";
        return 1;
    }
    kist::session_write_meta(session, domain_id, dds_uri, started);

    std::signal(SIGINT,  [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });
    std::printf("[kist_data_collector] recording %zu camera(s)%s + %zu hand(s) -> %s (domain=%d)\n",
                recorders.size(), lowstate ? " + lowstate" : "", hands.size(),
                session.dir.c_str(), domain_id);

    // 1 Hz per-stream report from the recorders' produce-site counters:
    // rx fps (delta received), wr fps (delta written), and the two loss
    // counters that must stay 0 (drop = our queue, gap = lost on the wire).
    struct Last { uint64_t c_rx = 0, c_wr = 0, d_rx = 0, d_wr = 0; };
    std::vector<Last> last(recorders.size());
    uint64_t last_ls_rx = 0, last_ls_wr = 0;
    uint64_t last_uw_rx = 0, last_uw_wr = 0;
    std::vector<std::pair<uint64_t, uint64_t>> last_hand(hands.size(), {0, 0});
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
                        human_size(c.bytes).c_str(),
                        (unsigned long long)(d.received - last[i].d_rx),
                        (unsigned long long)(d.written  - last[i].d_wr),
                        (unsigned long long)d.dropped, (unsigned long long)d.wire_gaps,
                        human_size(d.bytes).c_str());
            last[i] = {c.received, c.written, d.received, d.written};
        }
        if (lowstate) {
            const auto l = lowstate->stats();
            std::printf("  %-12s rx %4llu wr %4llu hz drop %llu werr %llu %s\n",
                        "lowstate",
                        (unsigned long long)(l.received - last_ls_rx),
                        (unsigned long long)(l.written  - last_ls_wr),
                        (unsigned long long)l.dropped, (unsigned long long)l.write_errors,
                        human_size(l.bytes).c_str());
            last_ls_rx = l.received;
            last_ls_wr = l.written;
        }
        if (uwb) {
            const auto u = uwb->stats();
            std::printf("  %-12s rx %4llu wr %4llu hz drop %llu werr %llu %s\n",
                        "uwb",
                        (unsigned long long)(u.received - last_uw_rx),
                        (unsigned long long)(u.written  - last_uw_wr),
                        (unsigned long long)u.dropped, (unsigned long long)u.write_errors,
                        human_size(u.bytes).c_str());
            last_uw_rx = u.received;
            last_uw_wr = u.written;
        }
        for (size_t i = 0; i < hands.size(); ++i) {
            const auto h = hands[i]->stats();
            std::printf("  hand_%-7s rx %4llu wr %4llu hz drop %llu werr %llu %s\n",
                        hands[i]->side().c_str(),
                        (unsigned long long)(h.received - last_hand[i].first),
                        (unsigned long long)(h.written  - last_hand[i].second),
                        (unsigned long long)h.dropped, (unsigned long long)h.write_errors,
                        human_size(h.bytes).c_str());
            last_hand[i] = {h.received, h.written};
        }
    }

    std::printf("[kist_data_collector] stopping — draining queues\n");
    std::vector<kist::StreamSummary> summary;
    for (auto& rec : recorders) {
        rec->stop();
        summary.push_back({rec->name(), "color", rec->color_stats()});
        summary.push_back({rec->name(), "depth", rec->depth_stats()});
    }
    if (lowstate) {
        lowstate->stop();
        summary.push_back({"unitree", "lowstate", lowstate->stats()});
    }
    for (auto& hand : hands) {
        hand->stop();
        summary.push_back({"dex3", "hand_" + hand->side(), hand->stats()});
    }
    if (uwb) {
        uwb->stop();
        summary.push_back({"uwb", "position", uwb->stats()});
    }
    kist::session_finalize_meta(session, summary);

    for (const auto& s : summary)
        std::printf("  %-12s %-5s received %llu written %llu dropped %llu "
                    "write_errors %llu wire_gaps %llu\n",
                    s.source.c_str(), s.stream.c_str(),
                    (unsigned long long)s.stats.received, (unsigned long long)s.stats.written,
                    (unsigned long long)s.stats.dropped, (unsigned long long)s.stats.write_errors,
                    (unsigned long long)s.stats.wire_gaps);
    std::printf("[kist_data_collector] session %s closed\n", session.dir.c_str());
    return 0;
}
